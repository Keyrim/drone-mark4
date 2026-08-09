/// @file
/// @brief Child process supervision implementation.

#include "hub/launcher.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mark4
{
    namespace
    {
        constexpr std::size_t PATH_BUFFER_SIZE = 4096U;
        constexpr long NS_PER_MS = 1'000'000L;

        /// @brief Sleeps without burning the processor.
        /// @param milliseconds delay [ms]
        void sleepMs(unsigned milliseconds)
        {
            timespec delay{};
            delay.tv_nsec = static_cast<long>(milliseconds) * NS_PER_MS;
            static_cast<void>(::nanosleep(&delay, nullptr));
        }

        /// @brief Turns a wait status into the exit code a shell would report.
        /// @param status status returned by waitpid
        /// @return the exit code
        int exitCodeOf(int status)
        {
            if (WIFEXITED(status))
            {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status))
            {
                return ProcessGroup::SIGNAL_EXIT_BASE + WTERMSIG(status);
            }
            return 0;
        }

        /// @brief Spawns one child, optionally as its own process group leader.
        /// @param spec what to run
        /// @param ownGroup true to make the child a process group leader
        /// @param pidOut receives the process id
        /// @return true when the child started
        bool spawnChild(const ChildSpec &spec, bool ownGroup, pid_t &pidOut)
        {
            std::vector<char *> argv;
            argv.reserve(spec.arguments.size() + 2U);
            argv.push_back(const_cast<char *>(spec.program.c_str()));
            for (const std::string &argument : spec.arguments)
            {
                argv.push_back(const_cast<char *>(argument.c_str()));
            }
            argv.push_back(nullptr);

            posix_spawnattr_t attributes;
            static_cast<void>(::posix_spawnattr_init(&attributes));
            if (ownGroup)
            {
                static_cast<void>(::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
                static_cast<void>(::posix_spawnattr_setpgroup(&attributes, 0));
            }
            const int failure = ::posix_spawnp(
                &pidOut, spec.program.c_str(), nullptr, &attributes, argv.data(), environ);
            static_cast<void>(::posix_spawnattr_destroy(&attributes));
            if (failure != 0)
            {
                static_cast<void>(std::fprintf(stderr,
                                               "hub: cannot start %s: %s\n",
                                               spec.program.c_str(),
                                               std::strerror(failure)));
                return false;
            }
            return true;
        }
    } // namespace

    std::string describeChild(const ChildSpec &spec)
    {
        std::string line = spec.program;
        for (const std::string &argument : spec.arguments)
        {
            line += " " + argument;
        }
        return line;
    }

    std::string executableDirectory()
    {
        std::array<char, PATH_BUFFER_SIZE> path{};
        const ssize_t size = ::readlink("/proc/self/exe", path.data(), path.size() - 1U);
        if (size <= 0)
        {
            return {};
        }
        return std::filesystem::path(std::string(path.data(), static_cast<std::size_t>(size)))
            .parent_path()
            .string();
    }

    std::string defaultBinaryPath(const char *appName)
    {
        const std::string directory = executableDirectory();
        if (!directory.empty())
        {
            const std::string candidate = directory + "/../" + appName + "/" + appName;
            std::error_code failure;
            if (std::filesystem::exists(candidate, failure))
            {
                return candidate;
            }
        }
        return std::string("build/desktop/apps/") + appName + "/" + appName;
    }

    std::string defaultProjectPath(const char *relative)
    {
        const std::string directory = executableDirectory();
        if (!directory.empty())
        {
            // build/<preset>/apps/hub is four levels below the source root.
            const std::string candidate = directory + "/../../../../" + relative;
            std::error_code failure;
            if (std::filesystem::exists(candidate, failure))
            {
                return candidate;
            }
        }
        return relative;
    }

    ProcessGroup::~ProcessGroup()
    {
        static_cast<void>(terminateAll());
    }

    bool ProcessGroup::spawn(const ChildSpec &spec)
    {
        pid_t pid = -1;
        if (!spawnChild(spec, true, pid))
        {
            return false;
        }
        m_children.push_back(Child{spec.program, pid, true});
        static_cast<void>(std::printf(
            "hub: started %s (pid %d)\n", describeChild(spec).c_str(), static_cast<int>(pid)));
        static_cast<void>(std::fflush(stdout));
        return true;
    }

    bool ProcessGroup::anyExited()
    {
        for (Child &child : m_children)
        {
            if (!child.running)
            {
                continue;
            }
            int status = 0;
            const pid_t reaped = ::waitpid(child.pid, &status, WNOHANG);
            if (reaped != child.pid)
            {
                continue;
            }
            child.running = false;
            const int code = exitCodeOf(status);
            static_cast<void>(
                std::printf("hub: %s exited with code %d\n", child.name.c_str(), code));
            static_cast<void>(std::fflush(stdout));
            if (!m_exitSeen)
            {
                m_exitSeen = true;
                m_firstExitCode = code;
            }
        }
        return m_exitSeen;
    }

    int ProcessGroup::terminateAll()
    {
        for (const Child &child : m_children)
        {
            if (child.running)
            {
                // The whole group, so a child's own children go too.
                static_cast<void>(::kill(-child.pid, SIGTERM));
            }
        }

        unsigned waited = 0U;
        while (waited < TERMINATE_GRACE_MS)
        {
            bool remaining = false;
            for (Child &child : m_children)
            {
                if (!child.running)
                {
                    continue;
                }
                int status = 0;
                if (::waitpid(child.pid, &status, WNOHANG) == child.pid)
                {
                    child.running = false;
                    if (!m_exitSeen)
                    {
                        m_exitSeen = true;
                        m_firstExitCode = exitCodeOf(status);
                    }
                }
                else
                {
                    remaining = true;
                }
            }
            if (!remaining)
            {
                break;
            }
            sleepMs(TERMINATE_POLL_MS);
            waited += TERMINATE_POLL_MS;
        }

        for (Child &child : m_children)
        {
            if (!child.running)
            {
                continue;
            }
            static_cast<void>(std::fprintf(
                stderr, "hub: %s ignored the stop request, killing it\n", child.name.c_str()));
            static_cast<void>(::kill(-child.pid, SIGKILL));
            int status = 0;
            static_cast<void>(::waitpid(child.pid, &status, 0));
            child.running = false;
        }
        m_children.clear();
        return m_firstExitCode;
    }

    bool runChildToCompletion(const ChildSpec &spec, int &exitCodeOut)
    {
        pid_t pid = -1;
        if (!spawnChild(spec, false, pid))
        {
            return false;
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0)
        {
            if (errno != EINTR)
            {
                return false;
            }
        }
        exitCodeOut = exitCodeOf(status);
        return true;
    }
} // namespace mark4
