#pragma once

/// @file
/// @brief The walk of one paged table, as the consumer that asks for it
///        keeps it: the items pulled so far, the cursor the next page
///        request carries, and the id of the request still outstanding.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mark4
{
    /// One table pulled page by page. The consumer sends the page request
    /// itself (it is the only one that knows the message type) and hands the
    /// id back with setRequestId(); this holds what came back. Fixed
    /// storage, no heap: MAX is what the composition affords for one node.
    ///
    /// @tparam T item of the table, as the wire describes it
    /// @tparam MAX items the pull keeps at most
    template <typename T, std::size_t MAX> class TablePull
    {
      public:
        /// @brief Forgets everything: no item, cursor 0, neither complete
        ///        nor abandoned, no request outstanding. What a fresh pull
        ///        of the same table starts from.
        void reset()
        {
            m_count = 0U;
            m_cursor = 0U;
            m_total = 0U;
            m_requestId = 0U;
            m_complete = false;
            m_abandoned = false;
        }

        /// @return true when the whole table arrived
        [[nodiscard]] bool complete() const
        {
            return m_complete;
        }

        /// @return true when a page request was given up on and the walk
        ///         stopped short
        [[nodiscard]] bool abandoned() const
        {
            return m_abandoned;
        }

        /// @return the cursor of the next page wanted
        [[nodiscard]] std::uint32_t cursor() const
        {
            return m_cursor;
        }

        /// @return items in the whole table, as the last page said; 0 before
        ///         one arrived
        [[nodiscard]] std::uint32_t total() const
        {
            return m_total;
        }

        /// @return the page request waiting for its answer, 0 when none
        [[nodiscard]] std::uint32_t requestId() const
        {
            return m_requestId;
        }

        /// @brief Remembers the page request the consumer just sent, so its
        ///        failure can be told apart from any other request's.
        /// @param id request id, 0 for none
        void setRequestId(std::uint32_t id)
        {
            m_requestId = id;
        }

        /// @return the items pulled so far, a dense prefix in table order
        [[nodiscard]] std::span<const T> items() const
        {
            return {m_items.data(), m_count};
        }

        /// @return the items pulled so far, writable: an item the node
        ///         describes again on its own (a log level that moved) is
        ///         replaced in place
        [[nodiscard]] std::span<T> items()
        {
            return {m_items.data(), m_count};
        }

        /// @return items pulled so far
        [[nodiscard]] std::size_t size() const
        {
            return m_count;
        }

        /// @return items the pull keeps at most
        [[nodiscard]] static constexpr std::size_t Capacity()
        {
            return MAX;
        }

        /// @brief Merges one page. A page whose cursor is not the one the
        ///        walk waits on is a duplicate or a stale answer and is
        ///        ignored; a page at cursor 0 restarts the table, so a node
        ///        that came back with fewer items keeps no stale entry. Past
        ///        MAX the rest of the page is dropped and the table is
        ///        marked complete at MAX: it is what this composition
        ///        affords, and asking for more would never end.
        /// @param total items in the whole table, as the page says
        /// @param pageCursor index of the first item of the page
        /// @param pageItems the items the page carries
        /// @return true when the page was merged
        bool applyPage(std::uint32_t total, std::uint32_t pageCursor, std::span<const T> pageItems)
        {
            if (pageCursor != m_cursor)
            {
                return false;
            }
            if (pageCursor == 0U)
            {
                m_count = 0U;
                m_complete = false;
            }
            m_total = total;
            m_requestId = 0U;
            bool overflow = false;
            for (const T &item : pageItems)
            {
                if (m_count == MAX)
                {
                    overflow = true;
                    break;
                }
                m_items[m_count] = item;
                ++m_count;
            }
            m_cursor += static_cast<std::uint32_t>(pageItems.size());
            // An empty page while the table is not full would loop the walk
            // forever on the same cursor: it closes the walk too.
            if (overflow || pageItems.empty() || m_cursor >= m_total)
            {
                m_complete = true;
            }
            return true;
        }

        /// @brief Gives up on the walk: the page request was never answered
        ///        and what was pulled stays as it is.
        void abandon()
        {
            m_abandoned = true;
            m_requestId = 0U;
        }

      private:
        std::array<T, MAX> m_items{};   ///< items pulled, a dense prefix
        std::size_t m_count = 0U;       ///< items in m_items
        std::uint32_t m_cursor = 0U;    ///< cursor of the next page wanted
        std::uint32_t m_total = 0U;     ///< items in the whole table
        std::uint32_t m_requestId = 0U; ///< page request outstanding, 0 when none
        bool m_complete = false;        ///< the whole table arrived
        bool m_abandoned = false;       ///< the walk was given up on
    };
} // namespace mark4
