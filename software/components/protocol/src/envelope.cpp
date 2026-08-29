#include "protocol/envelope.hpp"

#include <pb_decode.h>
#include <pb_encode.h>

namespace mark4
{
    bool encodeEnvelope(const mark4_Envelope &envelope,
                        std::uint8_t *out,
                        std::size_t capacity,
                        std::size_t &sizeOut)
    {
        if (out == nullptr || envelope.which_body == 0U)
        {
            return false;
        }
        pb_ostream_t stream = pb_ostream_from_buffer(out, capacity);
        if (!pb_encode(&stream, mark4_Envelope_fields, &envelope))
        {
            return false;
        }
        sizeOut = stream.bytes_written;
        return true;
    }

    bool decodeEnvelope(const std::uint8_t *data, std::size_t size, mark4_Envelope &envelopeOut)
    {
        envelopeOut = mark4_Envelope_init_zero;
        if (data == nullptr || size == 0U)
        {
            return false;
        }
        pb_istream_t stream = pb_istream_from_buffer(data, size);
        return pb_decode(&stream, mark4_Envelope_fields, &envelopeOut);
    }
} // namespace mark4
