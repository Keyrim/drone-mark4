#pragma once

/// @file
/// @brief Child process supervision, so one command brings a whole scenario
///        up and one Ctrl-C takes it back down. Every child is spawned into
///        a process group of its own: killing the group reaches whatever the
///        child itself started (a Godot editor spawns helpers), which killing
///        a single pid would leave behind. POSIX only.

#include <string>
#include <sys/types.h>
#include <vector>

namespace mark4
{
    /// One child to run: the binary and the arguments after argv[0].
    struct ChildSpec
    {
        std::string program;                ///< binary to execute
        std::vector<std::string> arguments; ///< arguments, argv[0] excluded
    };

    /// @brief Renders a spec as the command line it stands for, for logging.
    /// @param spec spec to render
    /// @return the command line
    std::string describeChild(const ChildSpec &spec);

    /// @brief Directory holding the running executable.
    /// @return the directory, empty when it cannot be determined
    std::string executableDirectory();

    /// @brief Default path of a sibling application binary, resolved next to
    ///        the running executable so a build tree anywhere works, and
    ///        falling back to the conventional path under the current
    ///        directory.
    /// @param appName application directory and binary name
    /// @return the path
    std::string defaultBinaryPath(const char *appName);

    /// @brief Default path of a directory of the source tree, resolved from
    ///        the running executable, falling back to the current directory.
    /// @param relative path relative to the source tree root
    /// @return the path
    std::string defaultProjectPath(const char *relative);

    /// The children of one scenario, alive together and dying together.
    class ProcessGroup
    {
      public:
        /// Time a child is given to honor a termination request [ms].
        static constexpr unsigned TERMINATE_GRACE_MS = 5000U;

        /// Delay between two checks while waiting for children to exit [ms].
        static constexpr unsigned TERMINATE_POLL_MS = 50U;

        /// Exit code offset reported for a child killed by a signal, as a
        /// shell reports it.
        static constexpr int SIGNAL_EXIT_BASE = 128;

        ProcessGroup() = default;
        ProcessGroup(const ProcessGroup &) = delete;
        ProcessGroup &operator=(const ProcessGroup &) = delete;
        ProcessGroup(ProcessGroup &&) = delete;
        ProcessGroup &operator=(ProcessGroup &&) = delete;
        ~ProcessGroup();

        /// @brief Starts one child in its own process group.
        /// @param spec what to run
        /// @return true when the child started
        bool spawn(const ChildSpec &spec);

        /// @brief Reaps whatever has exited since the last call.
        /// @return true when at least one child has exited
        bool anyExited();

        /// @brief Terminates every child still running: a termination request
        ///        first, then a kill for whoever ignored it.
        /// @return exit code of the first child that exited, 0 when none did
        int terminateAll();

        /// @return exit code of the first child that exited
        [[nodiscard]] int firstExitCode() const
        {
            return m_firstExitCode;
        }

      private:
        /// One spawned child.
        struct Child
        {
            std::string name;     ///< binary name, for the log lines
            pid_t pid = -1;       ///< process id, also its process group id
            bool running = false; ///< false once it has been reaped
        };

        std::vector<Child> m_children; ///< children of the scenario
        int m_firstExitCode = 0;       ///< exit code of the first one to go
        bool m_exitSeen = false;       ///< an exit code has been recorded
    };
} // namespace mark4
