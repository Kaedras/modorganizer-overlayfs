#include "overlayfs/overlayfsmanager.h"

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
  m_loglevel = level;
}

bool OverlayFsManager::isMounted() noexcept
{
  // lock in case a mount operation is pending
  scoped_lock mountLock(m_mountMutex);
  return m_mounted;
}

void OverlayFsManager::setWorkDir(const std::filesystem::path& directory,
                                  bool create) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  if (!exists(directory)) {
    if (create) {
      create_directories(directory);
      m_workDir = directory;
    } else {
      SPDLOG_ERROR("Directory does not exist");
    }
  } else {
    m_workDir = directory;
  }
}

void OverlayFsManager::setUpperDir(const std::filesystem::path& directory,
                                   bool create) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  if (!exists(directory)) {
    if (create) {
      create_directories(directory);
      m_upperDir = directory;
    } else {
      SPDLOG_ERROR("Directory does not exist");
    }
  } else {
    m_upperDir = directory;
  }
}

bool OverlayFsManager::addFile(const std::filesystem::path& source,
                               const std::filesystem::path& destination) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  if (is_directory(source)) {
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
  if (is_directory(destination)) {
    m_fileMap.emplace_back(source, destination / source.filename());
  } else {
    m_fileMap.emplace_back(source, destination);
  }
  return true;
}

bool OverlayFsManager::addDirectory(const std::filesystem::path& source,
                                    const std::filesystem::path& destination) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  if (!is_directory(source)) {
    m_logger->error("source must be a directory");
    return false;
  }
  if (!is_directory(destination)) {
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

std::vector<std::filesystem::path> OverlayFsManager::createOverlayFsDump() noexcept
{
  STUB();
  return {};
}

void OverlayFsManager::setLogFile(const std::filesystem::path& file) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_logFile = file;
  createLogger();
}

void OverlayFsManager::addSkipFileSuffix(const std::string& fileSuffix) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_fileSuffixBlacklist.emplace_back(fileSuffix);
}

void OverlayFsManager::clearSkipFileSuffixes() noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_fileSuffixBlacklist.clear();
}

void OverlayFsManager::addSkipDirectory(const std::filesystem::path& directory) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_directoryBlacklist.emplace_back(directory);
}

void OverlayFsManager::clearSkipDirectories() noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_directoryBlacklist.clear();
}

void OverlayFsManager::forceLoadLibrary(
    const std::filesystem::path& processName,
    const std::filesystem::path& libraryPath) noexcept
{
  scoped_lock dataLock(m_dataMutex);

  m_forceLoadLibraries.emplace_back(processName, libraryPath);
}

void OverlayFsManager::clearLibraryForceLoads() noexcept
{
  scoped_lock dataLock(m_dataMutex);

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
    string lowerDirs;

    m_logger->info(" . {}", i++);

    for (const auto& lowerDir : mount.lowerDirs) {
      m_logger->info("   . {} -> {}", lowerDir.generic_string(),
                     mount.target.generic_string());
      lowerDirs += lowerDir.string() + ":";
    }
    if (!mount.whiteout.empty()) {
      m_logger->info("ignored files/directories:");
      for (const auto& whiteout : mount.whiteout) {
        m_logger->info("   . {}", whiteout.generic_string());
      }
    }
    lowerDirs.pop_back();
  }

  m_logger->info("files:");

  for (const auto& file : m_fileMap) {
    m_logger->info(" . {} -> {}", file.source.generic_string(),
                   file.destination.generic_string());
  }
}

bool OverlayFsManager::mount() noexcept
{
  scoped_lock mountLock(m_mountMutex);
  scoped_lock dataLock(m_dataMutex);

  if (m_mounted) {
    return true;
  }

  if (!createOverlayFsMounts() || !processFiles()) {
    return false;
  }

  for (auto& mount : m_mounts) {
    // create lowerDirs string
    string lowerDirs;
    for (const std::filesystem::path& dir : mount.lowerDirs) {
      lowerDirs += dir.string() + ':';
    }
    // add destination to lowerDirs
    lowerDirs += mount.target.generic_string();

    if (mount.upperDir.empty() && !mount.whiteout.empty()) {
      m_logger->warn("cannot create whiteout files without upper dir");
    } else {
      // create whiteout files
      for (const auto& whiteout : mount.whiteout) {
        fs::path whiteoutFile = mount.upperDir / whiteout;
        // todo: store a list of created directories for later deletion
        create_directories(whiteoutFile.parent_path());
        // create a character device with device number 0/0
        int r = mknod(whiteoutFile.c_str(), S_IFCHR, makedev(0, 0));
        if (r != 0) {
          const int e = errno;
          m_logger->error("could not create whiteout file {}: {}",
                          whiteoutFile.string(), strerror(e));
          exit(1);
        }
        m_createdWhiteoutFiles.emplace_back(whiteoutFile);
      }
    }

    QProcess p;
    p.setProgram(u"fuse-overlayfs"_s);
    p.setProcessChannelMode(QProcess::MergedChannels);

    // create arguments
    QStringList args;
    args << u"--debug"_s;
    // the upper dir can be empty for read-only
    if (!mount.upperDir.empty()) {
      args << u"-o"_s << u"upperdir=\"%1\""_s.arg(mount.upperDir.c_str());
      args << u"-o"_s << u"workdir=\"%1\""_s.arg(mount.workDir.path());
    }
    args << u"-o"_s << u"lowerdir=\"%1\""_s.arg(lowerDirs.c_str());
    args << QString::fromStdString(mount.target.generic_string());

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
    args << u"-o"_s << u"lowerdir=\"%1\""_s.arg(mount.target.generic_string().c_str());
    args << QString::fromStdString(mount.target.generic_string());

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

bool OverlayFsManager::umount() noexcept
{
  scoped_lock mountLock(m_mountMutex);
  scoped_lock dataLock(m_dataMutex);

  if (!m_mounted) {
    m_logger->debug("umount: not mounted");
    return true;
  }

  if (m_mounts.empty() && m_fileMap.empty()) {
    m_logger->debug("umount: m_mounts and m_fileMap are empty");
    return true;
  }

  error_code ec;

  for (overlayFsData_t& entry : m_mounts) {
    // can be false on partial mounts
    if (!entry.mounted) {
      continue;
    }

    m_logger->debug("running \"umount {}\"", entry.target.generic_string());

    QProcess p;
    p.setProgram(u"umount"_s);
    p.setArguments({QString::fromStdString(entry.target.generic_string())});
    p.start();
    bool result = p.waitForFinished(timeout);

    if (!result || p.exitCode() != 0) {
      m_logger->error("umount returned {}", p.exitCode());
      return false;
    }
    m_logger->debug("umount {} success", entry.target.generic_string());
    entry.mounted = false;

    // delete whiteout files
    for (const std::filesystem::path& whiteout : entry.whiteout) {
      fs::path whiteoutLocation = entry.upperDir / whiteout;
      // check if the file is actually empty
      auto size = file_size(whiteoutLocation);
      if (size != 0) {
        m_logger->error("umount: whiteout file {} size should be 0, but is {}",
                        whiteoutLocation.generic_string(), size);
        continue;
      }
      filesystem::remove(whiteoutLocation, ec);
      if (ec) {
        m_logger->error("umount: could not remove whiteout file {}: ",
                        whiteoutLocation.generic_string(), ec.message());
      } else {
        m_logger->debug("umount: deleted whiteout file {}",
                        whiteoutLocation.generic_string());
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
    p.setArguments({QString::fromStdString(entry.target.generic_string())});
    p.start();
    bool result = p.waitForFinished(timeout);

    if (!result || p.exitCode() != 0) {
      m_logger->error("unmount returned {}: {}", p.exitCode(),
                      p.errorString().toStdString());
      return false;
    }
    m_logger->debug("unmount {} success", entry.target.generic_string());
    entry.mounted = false;
  }
  // symlinks are in a QTemporaryDir, so clearing this vector also removes them
  m_fileMounts.clear();

  m_fileMap.clear();

  m_mounted = false;
  return true;
}

bool OverlayFsManager::createProcess(const std::string& applicationName,
                                     const std::string& commandLine) noexcept
{
  scoped_lock dataLock(m_dataMutex);
  scoped_lock mountLock(m_mountMutex);

  if (!m_mounted) {
    if (!mount()) {
      m_logger->error("Not starting process because mount failed");
      return false;
    }
  }

  // todo: implement handling of m_forceLoadLibraries

  auto p = make_unique<QProcess>();
  p->setProgram(QString::fromStdString(applicationName));
  p->setArguments(QProcess::splitCommand(QString::fromStdString(commandLine)));
  p->start();
  if (p->waitForStarted()) {
    m_logger->debug("created process \"{} {}\" with pid {}", applicationName,
                    commandLine, p->processId());

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

OverlayFsManager::OverlayFsManager(std::filesystem::path file) noexcept
    : m_logFile(std::move(file))
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
  auto fileSink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(m_logFile);

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
  set<fs::path> destinations;
  for (const map_t& entry : m_fileMap) {
    if (!destinations.contains(entry.destination.parent_path())) {
      destinations.emplace(entry.destination.parent_path());
    }
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

  error_code ec;
  for (const auto& destination : destinations) {
    m_logger->debug("processing file destination {}", destination.generic_string());
    fileData_t data;
    data.target   = destination;
    data.upperDir = QTemporaryDir(absolute((destination / ".").parent_path()).c_str());
    data.workDir  = QTemporaryDir(absolute((destination / ".").parent_path()).c_str());

    m_logger->debug("created upper dir {}", data.upperDir.path().toStdString());
    m_logger->debug("created work dir {}", data.workDir.path().toStdString());

    // create symlinks
    for (const map_t& entry : m_fileMap) {
      if (entry.destination.parent_path() == destination) {
        fs::path symlinkPath =
            data.upperDir.path().toStdString() / entry.destination.filename();
        create_symlink(entry.source, symlinkPath, ec);
        if (ec) {
          m_logger->error("error creating symlink {}: {}", symlinkPath.generic_string(),
                          ec.message());
          return false;
        }
        m_logger->debug("created symlink {} to {}", symlinkPath.generic_string(),
                        entry.destination.generic_string());
      }
    }
    m_fileMounts.push_back(std::move(data));
  }

  return true;
}

bool OverlayFsManager::createOverlayFsMounts() noexcept
{
  // create sets of unique sources and destinations
  set<fs::path> sources;
  set<fs::path> destinations;
  for (const map_t& entry : m_map) {
    if (!sources.contains(entry.source)) {
      sources.emplace(entry.source);
    }
    if (!destinations.contains(entry.destination)) {
      destinations.emplace(entry.destination);
    }
  }

  // check if a source is also a destination
  for (const fs::path& source : sources) {
    if (destinations.contains(source)) {
      m_logger->error("source {} cannot simultaneously be a destination",
                      source.generic_string());
      return false;
    }
  }

  m_logger->debug("createOverlayFsMounts:");
  m_logger->debug(" . {} sources", sources.size());
  m_logger->debug(" . {} destinations", destinations.size());

  // group items by destination
  for (const fs::path& dst : destinations) {
    overlayFsData_t data;

    data.target = dst;
    // add all sources with this destination
    for (const auto& entry : m_map) {
      if (entry.destination == dst) {
        // add overwrite directory as upper dir
        if (entry.source.generic_string().ends_with("overwrite")) {
          data.upperDir = entry.source;
          continue;
        }
        data.lowerDirs.emplace_back(entry.source);
        // create whiteouts
        for (const auto& iter : fs::recursive_directory_iterator(entry.source)) {
          // check directory blacklist
          if (iter.is_directory()) {
            fs::path directoryName = relative(iter.path(), iter.path().parent_path());
            if (std::ranges::find(m_directoryBlacklist, directoryName.string()) !=
                m_directoryBlacklist.end()) {
              data.whiteout.emplace_back(relative(iter.path(), entry.source));
            }
          } else {
            // check file suffix blacklist
            string fileName = iter.path().filename().string();
            for (const string& suffix : m_fileSuffixBlacklist) {
              if (fileName.ends_with(suffix)) {
                data.whiteout.emplace_back(relative(iter.path(), entry.source));
              }
            }
          }
        }
      }
    }
    // reverse order of lower dirs to get correct priorities
    std::ranges::reverse(data.lowerDirs);

    if (data.upperDir.empty()) {
      // todo: find overwrite directory, set upperDir and workDir
      //  use m_upperDir for now
      data.upperDir = m_upperDir;
    }

    // The “workdir” needs to be an empty directory on the same filesystem as upperDir.
    // -> just create a QTemporaryDir on the upperDir parent path
    data.workDir = QTemporaryDir(absolute((data.upperDir / ".").parent_path()).c_str());
    m_logger->debug("created workdir {}", data.workDir.path().toStdString());

    m_mounts.push_back(std::move(data));
  }

  return true;
}

void OverlayFsManager::cleanup() noexcept
{
  for (const auto& file : m_createdWhiteoutFiles) {
    // check file size
    auto size = file_size(file);
    if (size != 0) {
      m_logger->error("umount: whiteout file {} size should be 0, but is {}",
                      file.generic_string(), size);
      continue;
    }
    remove(file);
  }
  m_createdWhiteoutFiles.clear();

  for (const auto& dir : m_createdDirectories) {
    if (fs::is_empty(dir)) {
      remove(dir);
    }
  }
  m_createdDirectories.clear();
}