#include "transport/node_id.hpp"

#include <cstdio>

namespace mark4
{
    std::uint32_t randomNodeId()
    {
        std::FILE *source = std::fopen("/dev/urandom", "rb");
        if (source == nullptr)
        {
            return 0U;
        }
        std::uint32_t id = 0U;
        const std::size_t read = std::fread(&id, sizeof(id), 1U, source);
        static_cast<void>(std::fclose(source));
        if (read != 1U)
        {
            return 0U;
        }
        return id == 0U ? 1U : id;
    }
} // namespace mark4
