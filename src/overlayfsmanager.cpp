#include "overlayfs/overlayfsmanager.h"

#include <QDirIterator>
#include <QProcess>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <wait.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#define STUB() std::cout << __FILE_NAME__ << ": " << __PRETTY_FUNCTION__ << ": STUB!\n"

// process wait timeout in msec
static inline constexpr int timeout = 10'000;

using namespace std;
namespace fs = std::filesystem;
using namespace Qt::StringLiterals;

void OverlayFsManager::setLogLevel(spdlog::level::level_enum level) noexcept
{
  m_logger->debug("setting log level to {}", spdlog::level::to_string_view(level));
  m_loglevel = level;
  m_logger->set_level(level);
}

bool OverlayFsManager::isMounted() noexcept
{
  // lock in case a mount operation is pending
  scoped_lock mountLock(m_mountMutex);
  return m_mounted;
}

void OverlayFsManager::setWorkDir(const QString& directory, bool create) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("setting work dir to '{}'", directory.toStdString());

  QDir dir(directory);
  if (!dir.exists()) {
    if (create) {
      if (!dir.mkpath(u"."_s)) {
        m_logger->error("Error creating directory {}", directory.toStdString());
        return;
      }
      m_workDir = directory;
    } else {
      m_logger->error("Directory '{}' does not exist", directory.toStdString());
    }
  } else {
    m_workDir = directory;
  }
}

void OverlayFsManager::setUpperDir(const QString& directory, bool create) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  QDir dir(directory);

  m_logger->debug("setting upper dir to '{}'", directory.toStdString());
  if (!dir.exists()) {
    if (create) {
      if (!dir.mkpath(u"."_s)) {
        m_logger->error("Error creating directory {}", directory.toStdString());
        return;
      }
      m_upperDir = directory;
    } else {
      m_logger->error("Directory '{}' does not exist", directory.toStdString());
    }
  } else {
    m_upperDir = directory;
  }
}

bool OverlayFsManager::addFile(const QString& source,
                               const QString& destination) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("adding file '{}' with destination '{}'", source.toStdString(),
                  destination.toStdString());

  QFileInfo sourceInfo(source);
  QFileInfo destinationInfo(destination);

  if (sourceInfo.isDir()) {
    m_logger->error("source file must not be a directory");
    return false;
  }

  // check if there is an entry in m_maps with an identical source and destination
  for (const map_t& entry : m_fileMap) {
    if (entry.source == source && entry.destination == destination) {
      return true;
    }
  }

  // append the file name if destination is a directory
  if (destinationInfo.isDir()) {
    m_fileMap.emplace_back(source, destination % "/"_L1 % source);
  } else {
    m_fileMap.emplace_back(source, destination);
  }
  return true;
}

bool OverlayFsManager::addDirectory(const QString& source,
                                    const QString& destination) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("adding directory '{}' with destination '{}'", source.toStdString(),
                  destination.toStdString());

  QFileInfo sourceInfo(source);
  QFileInfo destinationInfo(destination);

  if (!sourceInfo.exists()) {
    // create the source if it does not exist
    QDir(source).mkpath(u"."_s);
  } else if (!sourceInfo.isDir()) {
    m_logger->error("source must be a directory");
    return false;
  }

  if (!destinationInfo.exists()) {
    // create the destination if it does not exist
    QDir(destination).mkpath(u"."_s);
  } else if (!destinationInfo.isDir()) {
    m_logger->error("destination must be a directory");
    return false;
  }

  // check if there is an entry in m_maps with an identical source and destination
  for (const map_t& entry : m_map) {
    if (entry.source == source && entry.destination == destination) {
      return true;
    }
  }

  // create a new entry
  m_map.emplace_back(map_t{source, destination});
  return true;
}

QStringList OverlayFsManager::createOverlayFsDump() noexcept
{
  scoped_lock mountLock(m_mountMutex);
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("creating overlayfs dump");
  QStringList result;
  result.reserve(1000);

  bool wasMounted = m_mounted;

  if (!mountInternal()) {
    return result;
  }

  for (const auto& mount : m_mounts) {
    QDirIterator iter(mount.target, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
      result << iter.next();
    }
  }
  for (const auto& mount : m_fileMounts) {
    QDirIterator iter(mount.target, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
      result << iter.next();
    }
  }

  if (!wasMounted) {
    umountInternal();
  }

  result.shrink_to_fit();
  return result;
}

void OverlayFsManager::setLogFile(const QString& file) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("setting log file to '{}'", file.toStdString());
  m_logFile = file;
  createLogger();
}

void OverlayFsManager::addSkipFileSuffix(const QString& fileSuffix) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("added skip file suffix '{}'", fileSuffix.toStdString());
  m_fileSuffixBlacklist.emplace_back(fileSuffix);
}

void OverlayFsManager::clearSkipFileSuffixes() noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("clearing skip file suffixes");
  m_fileSuffixBlacklist.clear();
}

void OverlayFsManager::addSkipDirectory(const QString& directory) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("added skip directory '{}'", directory.toStdString());
  m_directoryBlacklist.emplace_back(directory);
}

void OverlayFsManager::clearSkipDirectories() noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("clearing skip directories");
  m_directoryBlacklist.clear();
}

void OverlayFsManager::forceLoadLibrary(const QString& processName,
                                        const QString& libraryPath) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("adding forced library '{}' for process '{}'",
                  libraryPath.toStdString(), processName.toStdString());
  m_forceLoadLibraries.emplace_back(processName, libraryPath);
}

void OverlayFsManager::clearLibraryForceLoads() noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logger->debug("clearing forced libraries");
  m_forceLoadLibraries.clear();
}

void OverlayFsManager::clearMappings() noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_map.clear();
  m_fileMap.clear();
}

void OverlayFsManager::dryrun() noexcept
{
  m_logger->info("would mount");

  if (m_map.empty()) {
    m_logger->info("nothing");
    return;
  }

  m_logger->info("");

  if (!createOverlayFsMounts()) {
    m_logger->error("error creating mounts");
    return;
  }

  m_logger->info("directories");
  int i = 0;
  for (const auto& mount : m_mounts) {
    QString lowerDirs;

    m_logger->info(" . {}", i++);

    for (const QString& lowerDir : mount.lowerDirs) {
      m_logger->info("   . {} -> {}", lowerDir.toStdString(),
                     mount.target.toStdString());
      lowerDirs += lowerDir % ":"_L1;
    }
    if (!mount.whiteout.empty()) {
      m_logger->info("ignored files/directories:");
      for (const auto& whiteout : mount.whiteout) {
        m_logger->info("   . {}", whiteout.toStdString());
      }
    }
    lowerDirs.chop(1);
  }

  m_logger->info("files:");

  for (const auto& file : m_fileMap) {
    m_logger->info(" . {} -> {}", file.source.toStdString(),
                   file.destination.toStdString());
  }
}

bool OverlayFsManager::mount() noexcept
{
  scoped_lock mountLock(m_mountMutex);
  scoped_lock dataLock(m_dataMutex);

  return mountInternal();
}

bool OverlayFsManager::umount() noexcept
{
  scoped_lock mountLock(m_mountMutex);
  scoped_lock dataLock(m_dataMutex);

  return umountInternal();
}

bool OverlayFsManager::createProcess(const QString& applicationName,
                                     const QString& commandLine) noexcept
{
  scoped_lock dataLock(m_dataMutex);
  scoped_lock mountLock(m_mountMutex);

  m_logger->debug("creating process '{}' with commandline '{}'",
                  applicationName.toStdString(), commandLine.toStdString());
  if (!m_mounted) {
    if (!mountInternal()) {
      m_logger->error("Not starting process because mount failed");
      return false;
    }
  }

  // todo: implement handling of m_forceLoadLibraries

  auto p = make_unique<QProcess>();
  p->setProgram(applicationName);
  p->setArguments(QProcess::splitCommand(commandLine));
  p->start();
  if (p->waitForStarted()) {
    m_logger->debug("created process with pid {}", p->processId());

    QObject::connect(p.get(), &QProcess::finished, [this] {
      m_logger->debug("process finished, unmounting");
      umount();
    });

    m_startedProcesses.emplace_back(std::move(p));
    return true;
  }

  m_logger->error("error creating process: {}", p->errorString().toStdString());
  return false;
}

const char* OverlayFsManager::ofsVersionString() noexcept
{
  return "1.0.0";
}

void OverlayFsManager::setDebugMode(bool value) noexcept
{
  m_debuggingMode = value;
}

std::vector<pid_t> OverlayFsManager::getOverlayFsProcessList() const noexcept
{
  vector<pid_t> pids;
  pids.reserve(m_startedProcesses.size());

  for (const auto& p : m_startedProcesses) {
    pids.push_back(static_cast<pid_t>(p->processId()));
  }

  return pids;
}

OverlayFsManager::OverlayFsManager(QString file) noexcept
    : m_loglevel(spdlog::level::warn), m_logFile(std::move(file))
{
  createLogger();
}

OverlayFsManager::~OverlayFsManager() noexcept
{
  if (m_mounted) {
    if (!umount()) {
      m_logger->error("OverlayFS Manager dtor could not call umount");
    }
  }
}

void OverlayFsManager::createLogger() noexcept
{
  auto stdoutSink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
  auto fileSink =
      std::make_shared<spdlog::sinks::basic_file_sink_mt>(m_logFile.toStdString());

  spdlog::sinks_init_list sink_list = {stdoutSink, fileSink};

  m_logger = spdlog::create<spdlog::sinks::dist_sink_mt>("multi_sink", sink_list);
  m_logger->set_pattern("%H:%M:%S.%e [%L] %v");
  m_logger->set_level(spdlog::level::debug);
}

bool OverlayFsManager::processFiles() noexcept
{
  // Handle files by creating a symlink in a temporary directory and
  // then mounting it into the destination. This reduces complexity and works
  // even if the destination files exist

  // create a set of unique destination directories
  set<QString> destinations;
  for (const map_t& entry : m_fileMap) {
    destinations.emplace(QFileInfo(entry.destination).absolutePath());
  }

  // check if a destination also exists in m_mounts
  for (const auto& mount : m_mounts) {
    if (destinations.contains(mount.target)) {
      // todo: implement handling of this case
      //  -> add to m_mounts
      m_logger->error("file destination must not exist in directory destinations");
      return false;
    }
  }

  for (const auto& destination : destinations) {
    m_logger->debug("processing file destination {}", destination.toStdString());
    fileData_t data;
    data.target = destination;

    QString templatePath = destination % "_tmp_XXXXXX"_L1;

    data.upperDir = QTemporaryDir(templatePath);
    data.workDir  = QTemporaryDir(templatePath);

    m_logger->debug("created upper dir {}", data.upperDir.path().toStdString());
    m_logger->debug("created work dir {}", data.workDir.path().toStdString());

    // create symlinks
    for (const map_t& entry : m_fileMap) {
      QFileInfo dstInfo(entry.destination);
      if (dstInfo.path() == destination) {
        QString symlinkPath = data.upperDir.path() % "/"_L1 % dstInfo.fileName();
        QFile symlinkFile(entry.source);
        if (!symlinkFile.link(symlinkPath)) {
          m_logger->error("error creating symlink {}: {}", symlinkPath.toStdString(),
                          symlinkFile.errorString().toStdString());
          return false;
        }
        m_logger->debug("created symlink {} to {}", symlinkPath.toStdString(),
                        entry.destination.toStdString());
      }
    }
    m_fileMounts.push_back(std::move(data));
  }

  return true;
}

bool OverlayFsManager::createOverlayFsMounts() noexcept
{
  // create sets of unique sources and destinations
  set<QString> sources;
  set<QString> destinations;
  for (const map_t& entry : m_map) {
    sources.emplace(entry.source);
    destinations.emplace(entry.destination);
  }

  // check if a source is also a destination
  for (const QString& source : sources) {
    if (destinations.contains(source)) {
      m_logger->error("source {} cannot simultaneously be a destination",
                      source.toStdString());
      return false;
    }
  }

  m_logger->debug("createOverlayFsMounts:");
  m_logger->debug(" . {} sources", sources.size());
  m_logger->debug(" . {} destinations", destinations.size());

  // group items by destination
  for (const QString& dst : destinations) {
    overlayFsData_t data;

    data.target = dst;
    // add all sources with this destination
    for (const auto& entry : m_map) {
      if (entry.destination == dst) {
        // add overwrite directory as upper dir
        if (entry.source.endsWith("overwrite"_L1)) {
          data.upperDir = entry.source;
          continue;
        }
        data.lowerDirs.emplace_back(entry.source);
        // create whiteouts
        QDirIterator iter(entry.source, QDirIterator::Subdirectories);
        while (iter.hasNext()) {
          QFileInfo info = iter.nextFileInfo();

          // check directory blacklist
          if (info.isDir()) {
            QString directoryName = info.dir().dirName();
            if (std::ranges::find(m_directoryBlacklist, directoryName) !=
                m_directoryBlacklist.end()) {
              data.whiteout.emplace_back(info.dir().relativeFilePath(entry.source));
            }
          } else {
            // check file suffix blacklist
            QString fileName = info.fileName();
            for (const QString& suffix : m_fileSuffixBlacklist) {
              if (fileName.endsWith(suffix)) {
                data.whiteout.emplace_back(info.dir().relativeFilePath(entry.source));
              }
            }
          }
        }
      }
    }
    // reverse order of lower dirs to get correct priorities
    std::ranges::reverse(data.lowerDirs);

    if (data.upperDir.isEmpty()) {
      // todo: find overwrite directory, set upperDir and workDir
      //  use m_upperDir for now
      data.upperDir = m_upperDir;
    }

    // The “workdir” needs to be an empty directory on the same filesystem as upperDir.
    // -> just create a QTemporaryDir on the upperDir parent path
    data.workDir =
        QTemporaryDir(QDir(data.upperDir).absoluteFilePath(u"_tmp_XXXXXX"_s));
    m_logger->debug("created workdir {}", data.workDir.path().toStdString());

    m_mounts.push_back(std::move(data));
  }

  return true;
}

void OverlayFsManager::cleanup() noexcept
{
  for (const auto& file : m_createdWhiteoutFiles) {
    QFile f(file);
    // check file size
    qint64 size = f.size();
    if (size != 0) {
      m_logger->error("umount: whiteout file {} size should be 0, but is {}",
                      file.toStdString(), size);
      continue;
    }
    f.remove();
  }
  m_createdWhiteoutFiles.clear();

  for (const auto& dir : m_createdDirectories) {
    QDir d(dir);
    if (d.isEmpty()) {
      d.removeRecursively();
    }
  }
  m_createdDirectories.clear();
}

bool OverlayFsManager::mountInternal()
{
  m_logger->debug("mounting");
  if (m_mounted) {
    m_logger->debug("already mounted");
    return true;
  }

  if (!createOverlayFsMounts() || !processFiles()) {
    m_logger->error("error processing mount info");
    return false;
  }

  for (auto& mount : m_mounts) {
    // create lowerDirs string
    QString lowerDirs;
    for (const QString& dir : mount.lowerDirs) {
      lowerDirs += dir % ":"_L1;
    }
    // add destination to lowerDirs
    lowerDirs += mount.target;

    if (mount.upperDir.isEmpty() && !mount.whiteout.empty()) {
      m_logger->warn("cannot create whiteout files without upper dir");
    } else {
      // create whiteout files
      for (const auto& whiteout : mount.whiteout) {
        QString whiteoutPath  = mount.upperDir % "/"_L1 % whiteout;
        fs::path whiteoutFile = whiteoutPath.toStdString();
        // todo: store a list of created directories for later deletion
        create_directories(whiteoutFile.parent_path());
        // create a character device with device number 0/0
        int r = mknod(whiteoutFile.c_str(), S_IFCHR, makedev(0, 0));
        if (r != 0) {
          const int e = errno;
          m_logger->error("could not create whiteout file {}: {}",
                          whiteoutFile.string(), strerror(e));
          return false;
        }
        m_createdWhiteoutFiles.emplace_back(whiteoutPath);
      }
    }

    QProcess p;
    p.setProgram(u"fuse-overlayfs"_s);
    p.setProcessChannelMode(QProcess::MergedChannels);

    // create arguments
    QStringList args;
    args << u"--debug"_s;
    // the upper dir can be empty for read-only
    if (!mount.upperDir.isEmpty()) {
      args << u"-o"_s << u"upperdir=\"%1\""_s.arg(mount.upperDir);
      args << u"-o"_s << u"workdir=\"%1\""_s.arg(mount.workDir.path());
    }
    args << u"-o"_s << u"lowerdir=\"%1\""_s.arg(lowerDirs);
    args << mount.target;

    p.setArguments(args);

    m_logger->debug("mounting overlay fs with command: {} {}",
                    p.program().toStdString(), p.arguments().join(' ').toStdString());

    p.start();
    if (!p.waitForFinished(timeout)) {
      m_logger->error("mount error: {}", p.errorString().toStdString());
      return false;
    }

    QString str = p.readAll();
    auto lines  = str.split('\n');

    for (const auto& line : lines) {
      if (!line.isEmpty()) {
        m_logger->info(line.toStdString());
      }
    }

    if (p.exitCode() != 0) {
      const int e = errno;
      m_logger->error("mount failed with exit code {}: {}, errno: {}", p.exitCode(),
                      p.errorString().toStdString(), strerror(e));
      return false;
    }

    mount.mounted = true;
  }

  for (auto& mount : m_fileMounts) {
    QProcess p;
    p.setProgram(u"fuse-overlayfs"_s);
    p.setProcessChannelMode(QProcess::MergedChannels);

    // create arguments
    QStringList args;
    args << u"--debug"_s;
    args << u"-o"_s << u"upperdir=\"%1\""_s.arg(mount.upperDir.path());
    args << u"-o"_s << u"workdir=\"%1\""_s.arg(mount.workDir.path());
    args << u"-o"_s << u"lowerdir=\"%1\""_s.arg(mount.target);
    args << mount.target;

    p.setArguments(args);

    m_logger->debug("mounting overlayFS with command: {} {}", p.program().toStdString(),
                    p.arguments().join(' ').toStdString());

    p.start();
    if (!p.waitForFinished(timeout)) {
      m_logger->error("mount error: {}", p.errorString().toStdString());
      return false;
    }

    QString str = p.readAll();
    auto lines  = str.split('\n');

    for (const auto& line : lines) {
      if (!line.isEmpty()) {
        m_logger->info(line.toStdString());
      }
    }

    if (p.exitCode() != 0) {
      const int e = errno;
      m_logger->error("mount failed with exit code {}: {}, errno: {}", p.exitCode(),
                      p.errorString().toStdString(), strerror(e));
      return false;
    }

    mount.mounted = true;
  }

  m_mounted = true;
  return true;
}

bool OverlayFsManager::umountInternal()
{
  m_logger->debug("unmounting");
  if (!m_mounted) {
    m_logger->debug("not mounted");
    return true;
  }

  if (m_mounts.empty() && m_fileMap.empty()) {
    m_logger->debug("m_mounts and m_fileMap are empty");
    return true;
  }

  error_code ec;

  for (overlayFsData_t& entry : m_mounts) {
    // can be false on partial mounts
    if (!entry.mounted) {
      continue;
    }

    m_logger->debug("running \"umount {}\"", entry.target.toStdString());

    QProcess p;
    p.setProgram(u"umount"_s);
    p.setArguments({entry.target});
    p.start();
    bool result = p.waitForFinished(timeout);

    if (!result || p.exitCode() != 0) {
      m_logger->error("umount returned {}", p.exitCode());
      return false;
    }
    m_logger->debug("umount {} success", entry.target.toStdString());
    entry.mounted = false;

    // delete whiteout files
    for (const QString& whiteout : entry.whiteout) {
      QString whiteoutLocation(entry.upperDir % "/"_L1 % whiteout);
      QFile whiteoutFile(whiteoutLocation);

      // check if the file is actually empty
      auto size = whiteoutFile.size();
      if (size != 0) {
        m_logger->error("[umount] whiteout file {} size should be 0, but is {}",
                        whiteoutLocation.toStdString(), size);
        continue;
      }
      if (!whiteoutFile.remove()) {
        m_logger->error("[umount] could not remove whiteout file {}: ",
                        whiteoutLocation.toStdString(),
                        whiteoutFile.errorString().toStdString());
      } else {
        m_logger->debug("[umount] deleted whiteout file {}",
                        whiteoutLocation.toStdString());
      }
    }
  }
  m_mounts.clear();

  for (auto& entry : m_fileMounts) {
    if (!entry.mounted) {
      continue;
    }
    QProcess p;
    p.setProgram(u"umount"_s);
    p.setArguments({entry.target});
    p.start();
    bool result = p.waitForFinished(timeout);

    if (!result || p.exitCode() != 0) {
      m_logger->error("unmount returned {}: {}", p.exitCode(),
                      p.errorString().toStdString());
      return false;
    }
    m_logger->debug("unmount {} success", entry.target.toStdString());
    entry.mounted = false;
  }
  // symlinks are in a QTemporaryDir, so clearing this vector also removes them
  m_fileMounts.clear();

  m_fileMap.clear();

  m_mounted = false;
  return true;
}
