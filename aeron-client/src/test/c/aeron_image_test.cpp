/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <exception>
#include <functional>
#include <string>
#include <limits>

#include <gtest/gtest.h>

#include "aeron_client_test_utils.h"

extern "C"
{
#include "aeron_image.h"
#include "concurrent/aeron_term_appender.h"
}

#define FILE_PAGE_SIZE (4 * 1024)

#define SUB_URI "aeron:udp?endpoint=localhost:24567"
#define STREAM_ID (101)
#define SESSION_ID (110)
#define REGISTRATION_ID (27)
#define SUBSCRIBER_POSITION_ID (49)

#define INITIAL_TERM_ID (1234)

using namespace aeron::test;

class ImageTest : public testing::Test
{
public:
    ImageTest()
    {
    }

    ~ImageTest() override
    {
        if (!m_filename.empty())
        {
            aeron_log_buffer_delete(m_image->log_buffer);
            aeron_image_delete(m_image);

            ::unlink(m_filename.c_str());
        }
    }

    int64_t createImage()
    {
        aeron_image_t *image = nullptr;
        aeron_log_buffer_t *log_buffer = nullptr;
        std::string filename = tempFileName();

        createLogFile(filename);

        if (aeron_log_buffer_create(&log_buffer, filename.c_str(), m_correlationId, false) < 0)
        {
            throw std::runtime_error("could not create log_buffer: " + std::string(aeron_errmsg()));
        }

        if (aeron_image_create(
            &image,
            nullptr,
            nullptr,
            log_buffer,
            SUBSCRIBER_POSITION_ID,
            &m_sub_pos,
            m_correlationId,
            (int32_t)m_correlationId,
            "none",
            strlen("none")) < 0)
        {
            throw std::runtime_error("could not create image: " + std::string(aeron_errmsg()));
        }

        aeron_logbuffer_metadata_t *metadata =
            (aeron_logbuffer_metadata_t *)log_buffer->mapped_raw_log.log_meta_data.addr;

        m_image = image;
        m_filename = filename;

        m_term_length = metadata->term_length;
        m_initial_term_id = metadata->initial_term_id;
        m_position_bits_to_shift = (size_t)aeron_number_of_trailing_zeroes(metadata->term_length);

        return m_correlationId++;
    }

    void appendMessage(int64_t position, size_t length)
    {
        aeron_logbuffer_metadata_t *metadata =
            (aeron_logbuffer_metadata_t *)m_image->log_buffer->mapped_raw_log.log_meta_data.addr;
        const size_t index = aeron_logbuffer_index_by_position(position, m_position_bits_to_shift);
        uint8_t buffer[1024];
        int32_t term_id = aeron_logbuffer_compute_term_id_from_position(
            position, m_position_bits_to_shift, m_initial_term_id);
        int32_t tail_offset = (int32_t)position & (m_term_length - 1);

        metadata->term_tail_counters[index] = static_cast<int64_t>(term_id) << 32 | tail_offset;

        aeron_term_appender_append_unfragmented_message(
            &m_image->log_buffer->mapped_raw_log.term_buffers[index],
            &metadata->term_tail_counters[index],
            buffer,
            length,
            nullptr,
            nullptr,
            aeron_logbuffer_compute_term_id_from_position(position, m_position_bits_to_shift, m_initial_term_id),
            SESSION_ID,
            STREAM_ID);
    }

    static void fragment_handler(void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        auto image = reinterpret_cast<ImageTest *>(clientd);

        if (image->m_handler)
        {
            image->m_handler(buffer, length, header);
        }
    }

    static aeron_controlled_fragment_handler_action_t controlled_fragment_handler(
        void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        auto image = reinterpret_cast<ImageTest *>(clientd);

        if (image->m_controlled_handler)
        {
            return image->m_controlled_handler(buffer, length, header);
        }

        return AERON_ACTION_CONTINUE;
    }

    template<typename F>
    int imagePoll(F &&handler, size_t fragment_limit)
    {
        m_handler = handler;
        return aeron_image_poll(m_image, fragment_handler, this, fragment_limit);
    }

    template<typename F>
    int imageControlledPoll(F &&handler, size_t fragment_limit)
    {
        m_controlled_handler = handler;
        return aeron_image_controlled_poll(m_image, controlled_fragment_handler, this, fragment_limit);
    }

    template<typename F>
    int imageBoundedPoll(F &&handler, int64_t max_position, size_t fragment_limit)
    {
        m_handler = handler;
        return aeron_image_bounded_poll(m_image, fragment_handler, this, max_position, fragment_limit);
    }

    template<typename F>
    int imageBoundedControlledPoll(F &&handler, int64_t max_position, size_t fragment_limit)
    {
        m_controlled_handler = handler;
        return aeron_image_bounded_controlled_poll(
            m_image, controlled_fragment_handler, this, max_position, fragment_limit);
    }

    const uint8_t *termBuffer(size_t index)
    {
        return m_image->log_buffer->mapped_raw_log.term_buffers[index].addr;
    }

protected:
    int64_t m_correlationId = 0;
    int64_t m_sub_pos = 0;

    int32_t m_term_length = 0;
    int32_t m_initial_term_id = 0;

    size_t m_position_bits_to_shift = 0;

    std::function<void(const uint8_t *, size_t, aeron_header_t *)> m_handler = nullptr;
    std::function<aeron_controlled_fragment_handler_action_t(const uint8_t *, size_t, aeron_header_t *)>
        m_controlled_handler = nullptr;

    aeron_image_t *m_image = nullptr;
    std::string m_filename;
};

TEST_F(ImageTest, shouldReadFirstMessage)
{
    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    createImage();

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        EXPECT_EQ(buffer, termBuffer(0) + m_sub_pos + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
        EXPECT_EQ(header->frame->frame_header.type, AERON_HDR_TYPE_DATA);
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, alignedMessageLength);
}

TEST_F(ImageTest, shouldNotReadPastTail)
{
    createImage();

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
    {
        FAIL() << "should not be called";
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 0);
    EXPECT_EQ(static_cast<size_t>(m_sub_pos), 0u);
}

TEST_F(ImageTest, shouldReadOneLimitedMessage)
{
    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    createImage();

    appendMessage(m_sub_pos, messageLength);
    appendMessage(m_sub_pos + alignedMessageLength, messageLength);

    auto null_handler = [&](const uint8_t *, size_t length, aeron_header_t *header) {};

    EXPECT_EQ(imagePoll(null_handler, 1), 1);
}

TEST_F(ImageTest, shouldReadMultipleMessages)
{
    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    createImage();

    appendMessage(m_sub_pos, messageLength);
    appendMessage(m_sub_pos + alignedMessageLength, messageLength);
    size_t handlerCallCount = 0;

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
    {
        handlerCallCount++;
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 2);
    EXPECT_EQ(handlerCallCount, 2u);
    EXPECT_EQ(m_sub_pos, alignedMessageLength * 2);
}

TEST_F(ImageTest, shouldReadLastMessage)
{
    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    createImage();

    m_sub_pos = m_term_length - alignedMessageLength;

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        EXPECT_EQ(buffer, termBuffer(0) + m_sub_pos + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, m_term_length);
}

TEST_F(ImageTest, shouldNotReadLastMessageWhenPadding)
{
    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    createImage();

    m_sub_pos = m_term_length - alignedMessageLength;

    // this will append padding instead of the message as it will trip over the end.
    appendMessage(m_sub_pos, messageLength + AERON_DATA_HEADER_LENGTH);

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
    {
        FAIL() << "should not be called";
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 0);
    EXPECT_EQ(m_sub_pos, m_term_length);
}

TEST_F(ImageTest, shouldAllowValidPosition)
{
    createImage();

    int64_t expectedPosition = m_term_length - 32;

    m_sub_pos = expectedPosition;
    ASSERT_EQ(aeron_image_position(m_image), expectedPosition);

    ASSERT_EQ(aeron_image_set_position(m_image, m_term_length), 0);
    EXPECT_EQ(aeron_image_position(m_image), m_term_length);
}

TEST_F(ImageTest, shouldNotAdvancePastEndOfTerm)
{
    createImage();

    int64_t expectedPosition = m_term_length - 32;

    m_sub_pos = expectedPosition;
    ASSERT_EQ(aeron_image_position(m_image), expectedPosition);

    ASSERT_EQ(aeron_image_set_position(m_image, m_term_length + 32), -1);
    EXPECT_EQ(aeron_image_position(m_image), expectedPosition);
}

TEST_F(ImageTest, shouldReportCorrectPositionOnReception)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header) {};

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, alignedMessageLength);
}

TEST_F(ImageTest, shouldReportCorrectPositionOnReceptionWithNonZeroPositionInInitialTermId)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialTermOffset = 5 * alignedMessageLength;
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, initialTermOffset, m_position_bits_to_shift, INITIAL_TERM_ID);

    m_sub_pos = initialPosition;

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        EXPECT_EQ(buffer, termBuffer(0) + initialTermOffset + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, initialPosition + alignedMessageLength);
}

TEST_F(ImageTest, shouldReportCorrectPositionOnReceptionWithNonZeroPositionInNonInitialTermId)
{
    createImage();

    const int32_t activeTermId = INITIAL_TERM_ID + 1;
    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialTermOffset = 5 * alignedMessageLength;
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        activeTermId, initialTermOffset, m_position_bits_to_shift, INITIAL_TERM_ID);

    m_sub_pos = initialPosition;

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        EXPECT_EQ(buffer, termBuffer(1) + initialTermOffset + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
    };

    EXPECT_EQ(imagePoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, initialPosition + alignedMessageLength);
}

TEST_F(ImageTest, shouldPollNoFragmentsToControlledFragmentHandler)
{
    createImage();

    bool called = false;
    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        called = true;
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 0);
    EXPECT_FALSE(called);
    EXPECT_EQ(static_cast<size_t>(m_sub_pos), 0u);
}

TEST_F(ImageTest, shouldPollOneFragmentToControlledFragmentHandlerOnContinue)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        EXPECT_EQ(buffer, termBuffer(0) + m_sub_pos + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
        EXPECT_EQ(header->frame->frame_header.type, AERON_HDR_TYPE_DATA);
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, alignedMessageLength);
}

TEST_F(ImageTest, shouldNotPollOneFragmentToControlledFragmentHandlerOnAbort)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, 0, m_position_bits_to_shift, INITIAL_TERM_ID);

    m_sub_pos = initialPosition;

    appendMessage(m_sub_pos, messageLength);

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        return AERON_ACTION_ABORT;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 0);
    EXPECT_EQ(m_sub_pos, initialPosition);
}

TEST_F(ImageTest, shouldPollOneFragmentToControlledFragmentHandlerOnBreak)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);

    appendMessage(m_sub_pos, messageLength);
    appendMessage(m_sub_pos + alignedMessageLength, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        aeron_header_values_t values;
        aeron_header_values(header, &values);

        EXPECT_EQ(buffer, termBuffer(0) + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
        EXPECT_EQ(header->frame->frame_header.type, AERON_HDR_TYPE_DATA);
        EXPECT_EQ(values.type, AERON_HDR_TYPE_DATA);
        return AERON_ACTION_BREAK;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, alignedMessageLength);
}

TEST_F(ImageTest, shouldPollFragmentsToControlledFragmentHandlerOnCommit)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    size_t fragmentCount = 0;

    appendMessage(m_sub_pos, messageLength);
    appendMessage(m_sub_pos + alignedMessageLength, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        fragmentCount++;

        if (1 == fragmentCount)
        {
            EXPECT_EQ(m_sub_pos, 0);

            EXPECT_EQ(buffer, termBuffer(0) + AERON_DATA_HEADER_LENGTH);
        }
        else if (2 == fragmentCount)
        {
            // testing current position here after first message commit
            EXPECT_EQ(m_sub_pos, alignedMessageLength);

            EXPECT_EQ(buffer, termBuffer(0) + alignedMessageLength + AERON_DATA_HEADER_LENGTH);
        }

        EXPECT_EQ(length, messageLength);
        return AERON_ACTION_COMMIT;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 2);
    EXPECT_EQ(m_sub_pos, alignedMessageLength * 2);
}

TEST_F(ImageTest, shouldUpdatePositionToEndOfCommittedFragmentOnCommit)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, 0, m_position_bits_to_shift, INITIAL_TERM_ID);
    size_t fragmentCount = 0;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength);
    appendMessage(initialPosition + (2 * alignedMessageLength), messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        fragmentCount++;

        EXPECT_EQ(length, messageLength);

        if (1 == fragmentCount)
        {
            EXPECT_EQ(m_sub_pos, initialPosition);

            EXPECT_EQ(buffer, termBuffer(0) + AERON_DATA_HEADER_LENGTH);
            return AERON_ACTION_CONTINUE;
        }
        else if (2 == fragmentCount)
        {
            // testing current position here after first message continue
            EXPECT_EQ(m_sub_pos, initialPosition);

            EXPECT_EQ(buffer, termBuffer(0) + alignedMessageLength + AERON_DATA_HEADER_LENGTH);
            return AERON_ACTION_COMMIT;
        }
        else if (3 == fragmentCount)
        {
            // testing current position here after second message commit
            EXPECT_EQ(m_sub_pos, initialPosition + (2 * alignedMessageLength));

            EXPECT_EQ(buffer, termBuffer(0) + (2 * alignedMessageLength) + AERON_DATA_HEADER_LENGTH);
            return AERON_ACTION_CONTINUE;
        }

        return AERON_ACTION_COMMIT;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 3);
    EXPECT_EQ(m_sub_pos, initialPosition + (3 * alignedMessageLength));
}

TEST_F(ImageTest, shouldPollFragmentsToControlledFragmentHandlerOnContinue)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, 0, m_position_bits_to_shift, INITIAL_TERM_ID);
    size_t fragmentCount = 0;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength);

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        fragmentCount++;

        if (1 == fragmentCount)
        {
            EXPECT_EQ(m_sub_pos, initialPosition);

            EXPECT_EQ(buffer, termBuffer(0) + AERON_DATA_HEADER_LENGTH);
        }
        else if (2 == fragmentCount)
        {
            // testing current position here after first message continue
            EXPECT_EQ(m_sub_pos, initialPosition);

            EXPECT_EQ(buffer, termBuffer(0) + alignedMessageLength + AERON_DATA_HEADER_LENGTH);
        }

        EXPECT_EQ(length, messageLength);
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageControlledPoll(handler, std::numeric_limits<size_t>::max()), 2);
    EXPECT_EQ(m_sub_pos, initialPosition + (2 * alignedMessageLength));
}

TEST_F(ImageTest, shouldPollNoFragmentsToBoundedControlledFragmentHandlerWithMaxPositionBeforeInitialPosition)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, 0, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = initialPosition - AERON_DATA_HEADER_LENGTH;
    bool called = false;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength);

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        called = true;
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_FALSE(called);
    EXPECT_EQ(imageBoundedControlledPoll(handler, maxPosition, std::numeric_limits<size_t>::max()), 0);
    EXPECT_EQ(m_sub_pos, initialPosition);
}

TEST_F(ImageTest, shouldPollFragmentsToBoundedControlledFragmentHandlerWithInitialOffsetNotZero)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, alignedMessageLength, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = initialPosition + alignedMessageLength;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength);

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageBoundedControlledPoll(handler, maxPosition, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, maxPosition);
}

TEST_F(ImageTest, shouldPollFragmentsToBoundedControlledFragmentHandlerWithMaxPositionBeforeNextMessage)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, 0, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = initialPosition + alignedMessageLength;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength);

    auto handler = [&](const uint8_t *, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageBoundedControlledPoll(handler, maxPosition, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, initialPosition + alignedMessageLength);
}

TEST_F(ImageTest, shouldPollFragmentsToBoundedFragmentHandlerWithMaxPositionBeforeNextMessage)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, 0, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = initialPosition + alignedMessageLength;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength);

    auto null_handler = [&](const uint8_t *, size_t length, aeron_header_t *header) {};

    EXPECT_EQ(imageBoundedPoll(null_handler, maxPosition, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, initialPosition + alignedMessageLength);
}

TEST_F(ImageTest, shouldPollFragmentsToBoundedControlledFragmentHandlerWithMaxPositionAfterEndOfTerm)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int32_t initialOffset = m_term_length - (2 * alignedMessageLength);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, initialOffset, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = initialPosition + m_term_length;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength * 2);  // will insert padding

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        EXPECT_EQ(buffer, termBuffer(0) + initialOffset + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageBoundedControlledPoll(handler, maxPosition, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, m_term_length);
}

TEST_F(ImageTest, shouldPollFragmentsToBoundedControlledFragmentHandlerWithMaxPositionAboveIntMaxValue)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int32_t initialOffset = m_term_length - (2 * alignedMessageLength);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, initialOffset, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1000L;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength * 2);  // will insert padding

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
        -> aeron_controlled_fragment_handler_action_t
    {
        EXPECT_EQ(buffer, termBuffer(0) + initialOffset + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
        return AERON_ACTION_CONTINUE;
    };

    EXPECT_EQ(imageBoundedControlledPoll(handler, maxPosition, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, m_term_length);
}

TEST_F(ImageTest, shouldPollFragmentsToBoundedFragmentHandlerWithMaxPositionAboveIntMaxValue)
{
    createImage();

    const size_t messageLength = 120;
    const int64_t alignedMessageLength =
        AERON_ALIGN(messageLength + AERON_DATA_HEADER_LENGTH, AERON_LOGBUFFER_FRAME_ALIGNMENT);
    const int32_t initialOffset = m_term_length - (2 * alignedMessageLength);
    const int64_t initialPosition = aeron_logbuffer_compute_position(
        INITIAL_TERM_ID, initialOffset, m_position_bits_to_shift, INITIAL_TERM_ID);
    const int64_t maxPosition = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1000L;

    m_sub_pos = initialPosition;

    appendMessage(initialPosition, messageLength);
    appendMessage(initialPosition + alignedMessageLength, messageLength * 2);  // will insert padding

    auto handler = [&](const uint8_t *buffer, size_t length, aeron_header_t *header)
    {
        EXPECT_EQ(buffer, termBuffer(0) + initialOffset + AERON_DATA_HEADER_LENGTH);
        EXPECT_EQ(length, messageLength);
    };

    EXPECT_EQ(imageBoundedPoll(handler, maxPosition, std::numeric_limits<size_t>::max()), 1);
    EXPECT_EQ(m_sub_pos, m_term_length);
}
