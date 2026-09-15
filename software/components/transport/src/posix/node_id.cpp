#include "transport/node_id.hpp"

#include <cstdio>

namespace mark4
{
    namespace
    {
        /// @brief Draws one 32-bit word from /dev/urandom.
        /// @return the word, never 0; 0 when the random source cannot be read
        std::uint32_t randomWord()
        {
            std::FILE *source = std::fopen("/dev/urandom", "rb");
            if (source == nullptr)
            {
                return 0U;
            }
            std::uint32_t word = 0U;
            const std::size_t read = std::fread(&word, sizeof(word), 1U, source);
            static_cast<void>(std::fclose(source));
            if (read != 1U)
            {
                return 0U;
            }
            return word == 0U ? 1U : word;
        }
    } // namespace

    std::uint32_t randomNodeId()
    {
        return randomWord();
    }

    std::uint32_t randomBootId()
    {
        return randomWord();
    }
} // namespace mark4
