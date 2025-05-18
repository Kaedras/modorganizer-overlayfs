#pragma once

#include "logging.h"
#include <QTemporaryDir>
#include <filesystem>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#ifndef EXPORT
#define EXPORT __attribute__((visibility("default")))
#endif

class QProcess;

namespace spdlog
{
class logger;
}

class EXPORT OverlayFsManager
{
public:
  static OverlayFsManager&
  getInstance(const std::filesystem::path& file = "overlayfs.log") noexcept
  {
    static OverlayFsManager instance(file);
    return instance;
  }

  OverlayFsManager(OverlayFsManager const&) = delete;
  void operator=(OverlayFsManager const&)   = delete;

  void setLogLevel(LogLevel level) noexcept;
  [[nodiscard]] bool isMounted() const noexcept;

  /**
   * @brief Sets workdir and optionally creates it if it does not exist.
   * @param directory Workdir to use. Must be on the same file system as the upper dir.
   * @param create create the directory if it does not exist
   */
  void setWorkDir(const std::filesystem::path& directory, bool create = false) noexcept;

  /**
   * @brief Sets the upper dir and optionally creates it if it does not exist.
   * @param directory Upper dir to use. Must be on the same file system as workdir.
   * @param create Create the directory if it does not exist.
   */
  void setUpperDir(const std::filesystem::path& directory,
                   bool create = false) noexcept;

  bool addFile(const std::filesystem::path& source,
               const std::filesystem::path& destination) noexcept;

  /**
   * @param source
   * @param destination
   */
  bool addDirectory(const std::filesystem::path& source,
                    const std::filesystem::path& destination) noexcept;

  /**
   * retrieves a readable representation of the overlay fs tree
   */
  std::vector<std::filesystem::path> createOverlayFsDump() noexcept;

  void setLogFile(const std::filesystem::path& file) noexcept;

  /**
   * @brief Adds a file suffix to a list to skip during file linking
   * .txt and some_file.txt are both valid file suffixes,
   * not to be confused with file extensions
   * @param fileSuffix A valid file suffix
   */
  void addSkipFileSuffix(const std::string& fileSuffix) noexcept;

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
  void addSkipDirectory(const std::filesystem::path& directory) noexcept;

  /**
   * @brief Clears the directory skip-list
   */
  void clearSkipDirectories() noexcept;

  /**
   * @brief Adds a library to be force loaded when the given process is injected
   */
  void forceLoadLibrary(const std::filesystem::path& processName,
                        const std::filesystem::path& libraryPath) noexcept;
  /**
   * @brief Clears all previous calls to ForceLoadLibrary
   */
  void clearLibraryForceLoads() noexcept;

  void clearMappings() noexcept;

  void dryrun() noexcept;

  bool mount() noexcept;
  bool umount() noexcept;

  // std::string getName();
  bool createProcess(const std::string& applicationName,
                     const std::string& commandLine) noexcept;
  static const char* ofsVersionString() noexcept;
  void setDebugMode(bool value) noexcept;
  static const char* logLevelToString(LogLevel lv) noexcept;

  /**
   * @brief Retrieve a list of all processes that were started
   */
  std::vector<pid_t> getOverlayFsProcessList() const noexcept;

private:
  struct map_t
  {
    std::filesystem::path source;
    std::filesystem::path destination;
  };

  using Map = std::vector<map_t>;

  struct overlayFsData_t
  {
    std::filesystem::path target;
    std::filesystem::path upperDir;
    QTemporaryDir workDir;
    std::vector<std::filesystem::path> lowerDirs;
    std::vector<std::filesystem::path> whiteout;
    bool mounted = false;
  };

  struct fileData_t
  {
    // target is also lowerDir
    std::filesystem::path target;
    QTemporaryDir upperDir;
    QTemporaryDir workDir;
    bool mounted = false;
  };

  explicit OverlayFsManager(std::filesystem::path file) noexcept;
  ~OverlayFsManager() noexcept;

  void createLogger() noexcept;
  bool processFiles() noexcept;
  bool createOverlayFsMounts() noexcept;
  /**
   * @brief Deletes all whiteout files
   */
  void cleanup() noexcept;

  LogLevel m_loglevel = LogLevel::Warning;
  Map m_map;
  Map m_fileMap;
  std::vector<std::string> m_fileSuffixBlacklist;
  std::vector<std::string> m_directoryBlacklist;
  std::vector<std::filesystem::path> m_createdWhiteoutFiles;
  std::vector<std::filesystem::path> m_createdDirectories;
  std::vector<std::unique_ptr<QProcess>> m_startedProcesses;
  std::vector<overlayFsData_t> m_mounts;
  std::vector<fileData_t> m_fileMounts;
  std::shared_ptr<spdlog::logger> m_logger;
  std::filesystem::path m_logFile;
  bool m_mounted = false;
  /** Enable debugging mode, can be very noisy. */
  bool m_debuggingMode = false;
  /**
   * A directory used internally by fuse-overlayfs.
   * Must be on the same file system as the upper dir.
   */
  std::filesystem::path m_workDir;
  /**
   * A directory merged on top of all the lower dirs
   * where all the changes done to the file system will be written.
   */
  std::filesystem::path m_upperDir;
};