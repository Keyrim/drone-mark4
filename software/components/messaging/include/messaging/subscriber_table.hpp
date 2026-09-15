#pragma once

/// @file
/// @brief The node ids a stream goes to: a fixed set, added to and removed
///        from by the provider that owns the stream.

#include <array>
#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// The subscribers of one stream: at most N node ids, no heap, no order.
    /// A provider holds one per stream it emits, adds a node that subscribed,
    /// removes it when it unsubscribes or goes down, and walks the table to
    /// emit. The capacity is a constant of the stream, small because a
    /// board's link pays every entry.
    ///
    /// @tparam N node ids the table holds at most
    template <std::size_t N>
    class SubscriberTable
    {
      public:
        /// @brief Adds one node, if it is not there already.
        /// @param id node id to add
        /// @return true when the node is in the table afterwards, whether it
        ///         was added now or was already there; false when the table
        ///         is full
        bool add(std::uint32_t id)
        {
            if (contains(id))
            {
                return true;
            }
            if (m_count == N)
            {
                return false;
            }
            m_ids[m_count] = id;
            ++m_count;
            return true;
        }

        /// @brief Removes one node. The entries stay a dense prefix: the last
        ///        one takes the freed slot, so the order carries no meaning.
        /// @param id node id to remove
        /// @return true when the node was in the table
        bool remove(std::uint32_t id)
        {
            for (std::size_t index = 0U; index < m_count; ++index)
            {
                if (m_ids[index] != id)
                {
                    continue;
                }
                m_ids[index] = m_ids[m_count - 1U];
                --m_count;
                return true;
            }
            return false;
        }

        /// @param id node id to look for
        /// @return true when the node is in the table
        [[nodiscard]] bool contains(std::uint32_t id) const
        {
            for (std::size_t index = 0U; index < m_count; ++index)
            {
                if (m_ids[index] == id)
                {
                    return true;
                }
            }
            return false;
        }

        /// @return node ids in the table
        [[nodiscard]] std::size_t size() const
        {
            return m_count;
        }

        /// @return true when nothing is subscribed
        [[nodiscard]] bool empty() const
        {
            return m_count == 0U;
        }

        /// @return node ids the table holds at most
        [[nodiscard]] static constexpr std::size_t capacity()
        {
            return N;
        }

        /// @param index position in the table, 0 <= index < size()
        /// @return the node id at that position; the order carries no
        ///         meaning and changes on a removal
        [[nodiscard]] std::uint32_t id(std::size_t index) const
        {
            return m_ids[index];
        }

        /// @brief Drops every subscriber.
        void clear()
        {
            m_count = 0U;
        }

      private:
        std::array<std::uint32_t, N> m_ids{}; ///< the subscribers, a dense prefix
        std::size_t m_count = 0U;             ///< entries used in m_ids
    };
} // namespace mark4
