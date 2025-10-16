#pragma once

#include <QTemporaryDir>
#include <filesystem>
#include <vector>

#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

// forward declarations
class QProcess;
namespace spdlog
{
namespace level
{
  enum level_enum : int;
}
class logger;
}  // namespace spdlog

class EXPORT OverlayFsManager
{
public:
  static OverlayFsManager&
  getInstance(const QString& file = QStringLiteral("overlayfs.log")) noexcept
  {
    static OverlayFsManager instance(file);
    return instance;
  }

  OverlayFsManager(OverlayFsManager const&) = delete;
  void operator=(OverlayFsManager const&)   = delete;

  void setLogLevel(spdlog::level::level_enum level) noexcept;
  [[nodiscard]] bool isMounted() noexcept;

  /**
   * @brief Sets workdir and optionally creates it if it does not exist.
   * @param directory Workdir to use. Must be on the same file system as the upper dir.
   * @param create create the directory if it does not exist
   */
  void setWorkDir(const QString& directory, bool create = false) noexcept;

  /**
   * @brief Sets the upper dir and optionally creates it if it does not exist.
   * @param directory Upper dir to use. Must be on the same file system as workdir.
   * @param create Create the directory if it does not exist.
   */
  void setUpperDir(const QString& directory, bool create = false) noexcept;

  bool addFile(const QString& source, const QString& destination) noexcept;

  /**
   * @param source
   * @param destination
   */
  bool addDirectory(const QString& source, const QString& destination) noexcept;

  /**
   * retrieves a readable representation of the overlay fs tree
   */
  QStringList createOverlayFsDump() noexcept;

  void setLogFile(const QString& file) noexcept;

  /**
   * @brief Adds a file suffix to a list to skip during file linking
   * .txt and some_file.txt are both valid file suffixes,
   * not to be confused with file extensions
   * @param fileSuffix A valid file suffix
   */
  void addSkipFileSuffix(const QString& fileSuffix) noexcept;

  /**
   * @brief Clears the file suffix skip-list
   */
  void clearSkipFileSuffixes() noexcept;

  /**
   * @brief Adds a directory name that will be skipped during directory linking.
   * Not a path. Any directory matching the name will be skipped,
   * regardless of its path, for example, if .git is added,
   * any sub-path or root-path containing a .git directory
   * will have the .git directory skipped during directory linking
   * @param directory Name of the directory
   */
  void addSkipDirectory(const QString& directory) noexcept;

  /**
   * @brief Clears the directory skip-list
   */
  void clearSkipDirectories() noexcept;

  /**
   * @brief Adds a library to be force loaded when the given process is injected
   */
  void forceLoadLibrary(const QString& processName,
                        const QString& libraryPath) noexcept;
  /**
   * @brief Clears all previous calls to ForceLoadLibrary
   */
  void clearLibraryForceLoads() noexcept;

  void clearMappings() noexcept;

  void dryrun() noexcept;

  bool mount() noexcept;
  bool umount() noexcept;

  /**
   * @brief Creates and starts a new process after ensuring that the overlay filesystem
   * is properly mounted. Automatically unmounts the filesystem when the process
   * terminates.
   * @param applicationName The name or path of the executable to be run as a process.
   * @param commandLine The command-line arguments to be passed to the executable.
   * @return true if the process was successfully created and started; false otherwise.
   */
  bool createProcess(const QString& applicationName,
                     const QString& commandLine) noexcept;
  static const char* ofsVersionString() noexcept;
  void setDebugMode(bool value) noexcept;

  /**
   * @brief Retrieve a list of all processes that were started
   */
  [[nodiscard]] std::vector<pid_t> getOverlayFsProcessList() const noexcept;

private:
  struct map_t
  {
    QFileInfo source;
    QFileInfo destination;
  };
  using Map = std::vector<map_t>;

  struct forceLoadLibrary_t
  {
    QString processName;
    QString libraryPath;
  };

  struct overlayFsData_t
  {
    QString target;
    QString upperDir;
    QTemporaryDir workDir;
    QStringList lowerDirs;
    QStringList whiteout;
    bool mounted = false;
    std::vector<QTemporaryDir> tmpDirs;
  };

  explicit OverlayFsManager(QString file) noexcept;
  ~OverlayFsManager() noexcept;

  void createLogger() noexcept;
  [[nodiscard]] bool prepareMounts() noexcept;
  [[nodiscard]] bool createSymlinks() noexcept;

  /**
   * @brief Deletes all whiteout files
   */
  void cleanup() noexcept;

  // mount functions without locks for internal use
  [[nodiscard]] bool mountInternal();
  [[nodiscard]] bool umountInternal();

  [[nodiscard]] bool isAnythingMounted() const noexcept;

  /**
   * @brief Creates the specified directory including parent directories and store all
   * created directories in m_createdDirectories
   */
  [[nodiscard]] bool createDirectories(const std::string& directory) noexcept;
  [[nodiscard]] bool createDirectories(const QString& directory) noexcept;

  spdlog::level::level_enum m_loglevel;
  Map m_map;
  Map m_fileMap;
  std::vector<forceLoadLibrary_t> m_forceLoadLibraries;
  QStringList m_fileSuffixBlacklist;
  QStringList m_directoryBlacklist;
  QStringList m_createdWhiteoutFiles;
  QStringList m_createdDirectories;
  QStringList m_createdSymlinks;
  std::vector<std::unique_ptr<QProcess>> m_startedProcesses;
  std::vector<overlayFsData_t> m_mounts;
  std::shared_ptr<spdlog::logger> m_logger;
  QString m_logFile;
  bool m_mounted = false;
  std::mutex m_mountMutex;
  std::mutex m_dataMutex;
  /** Enable debugging mode, can be very noisy. */
  bool m_debuggingMode = false;
  /**
   * A directory used internally by fuse-overlayfs.
   * Must be on the same file system as the upper dir.
   */
  QString m_workDir;
  /**
   * A directory merged on top of all the lower dirs
   * where all the changes done to the file system will be written.
   */
  QString m_upperDir;
};
