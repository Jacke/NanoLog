/* Copyright (c) 2016 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <unistd.h>
#include <fcntl.h>
#include <iosfwd>
#include <locale>
#include <stdlib.h>

#include "FastLogger.h"

// BufferStuffer.h is a file generated by the FastLogger preprocessor which
// contains all the compression and decompression functions.
#include "BufferStuffer.h"
#include "Cycles.h"

namespace PerfUtils {

// Define the static members of FastLogger here
__thread FastLogger::StagingBuffer* FastLogger::stagingBuffer = nullptr;
thread_local FastLogger::StagingBufferDestroyer FastLogger::sbc;
FastLogger FastLogger::fastLogger;

// FastLogger constructor
FastLogger::FastLogger()
    : threadBuffers()
    , bufferMutex()
    , compressionThread()
    , hasOutstandingOperation(false)
    , compressionThreadShouldExit(false)
    , syncRequested(false)
    , condMutex()
    , workAdded()
    , hintQueueEmptied()
    , outputFd(-1)
    , aioCb()
    , compressingBuffer(nullptr)
    , outputDoubleBuffer(nullptr)
    , cycleAtThreadStart(0)
    , cyclesAwake(0)
    , cyclesCompressing(0)
    , cyclesScanningAndCompressing(0)
    , cyclesAioAndFsync(0)
    , totalBytesRead(0)
    , totalBytesWritten(0)
    , padBytesWritten(0)
    , eventsProcessed(0)
    , numAioWritesCompleted(0)
{
    outputFd = open("/tmp/compressedLog", FILE_PARAMS);
    if (!outputFd) {
        fprintf(stderr, "FastLogger could not open the default file location "
                "for the log file (\"%s\").\r\n Please check the permissions "
                "or use FastLogger::setLogFile(const char* filename) to "
                "specify a different log file.\r\n", "/tmp/compressedLog");
        std::exit(-1);
    }

    memset(&aioCb, 0, sizeof(aioCb));

    int err = posix_memalign(reinterpret_cast<void**>(&compressingBuffer),
                                                    512, OUTPUT_BUFFER_SIZE);
    if (err) {
        perror("The FastLogger system was not able to allocate enough memory "
                "to support its operations. Quitting...\r\n");
        std::exit(-1);
    }

    err = posix_memalign(reinterpret_cast<void**>(&outputDoubleBuffer),
                                                    512, OUTPUT_BUFFER_SIZE);
    if (err) {
        perror("The FastLogger system was not able to allocate enough memory "
                "to support its operations. Quitting...\r\n");
        std::exit(-1);
    }

    compressionThread = std::thread(&FastLogger::compressionThreadMain, this);
}

// FastLogger destructor
FastLogger::~FastLogger()
{
    sync();

    // Stop the compression thread
    {
        std::lock_guard<std::mutex> lock(fastLogger.condMutex);
        fastLogger.compressionThreadShouldExit = true;
        fastLogger.workAdded.notify_all();
    }

    if (fastLogger.compressionThread.joinable())
        fastLogger.compressionThread.join();

    // Free all the data structures
    if (compressingBuffer) {
        free(compressingBuffer);
        compressingBuffer = nullptr;
    }

    if (outputDoubleBuffer) {
        free(outputDoubleBuffer);
        outputDoubleBuffer = nullptr;
    }

    if (outputFd > 0)
        close(outputFd);

    outputFd = 0;
}

/**
 * User API: Print various statistics gathered by the FastLogger system to
 * stdout. This is primarily intended as a performance debugging aid.
 */
void
FastLogger::printStats()
{
   // Leaks abstraction, but basically flush so we get all the time
    uint64_t start = Cycles::rdtsc();
    fdatasync(fastLogger.outputFd);
    uint64_t stop = Cycles::rdtsc();
    fastLogger.cyclesAioAndFsync += (stop - start);

    double outputTime = Cycles::toSeconds(fastLogger.cyclesAioAndFsync);
    double compressTime = Cycles::toSeconds(fastLogger.cyclesCompressing);
    double workTime = outputTime + compressTime;

    double totalBytesWrittenDouble = static_cast<double>(
                                                fastLogger.totalBytesWritten);
    double totalBytesReadDouble = static_cast<double>(
                                                fastLogger.totalBytesRead);
    double padBytesWrittenDouble = static_cast<double>(
                                                fastLogger.padBytesWritten);
    double numEventsProcessedDouble = static_cast<double>(
                                                fastLogger.eventsProcessed);

    printf("Wrote %lu events (%0.2lf MB) in %0.3lf seconds "
            "(%0.3lf seconds spent compressing)\r\n",
            fastLogger.eventsProcessed,
            totalBytesWrittenDouble/1.0e6,
            workTime,
            compressTime);

    printf("There were %u file flushes and the final sync time was %lf sec\r\n",
            fastLogger.numAioWritesCompleted, Cycles::toSeconds(stop - start));

    double secondsAwake = Cycles::toSeconds(fastLogger.cyclesAwake);
    double secondsThreadHasBeenAlive = Cycles::toSeconds(
                            Cycles::rdtsc() - fastLogger.cycleAtThreadStart);
    printf("Compression Thread was active for %0.3lf out of %0.3lf seconds "
            "(%0.2lf %%)\r\n",
            secondsAwake,
            secondsThreadHasBeenAlive,
            100.0*secondsAwake/secondsThreadHasBeenAlive);

    printf("On average, that's\r\n"
            "\t%0.2lf MB/s or %0.2lf ns/byte w/ processing\r\n",
                (totalBytesWrittenDouble/1.0e6)/(workTime),
                (workTime*1.0e9)/totalBytesWrittenDouble);

    if (!FastLogger::USE_AIO) {
        // Since we can't reliably measure raw output speeds with AIO,
        // we don't print it.
        printf("\t%0.2lf MB/s or %0.2lf ns/byte raw output\r\n",
                (totalBytesWrittenDouble/1.0e6)/outputTime,
                (outputTime)*1.0e9/totalBytesWrittenDouble);
    }

    printf("\t%0.2lf MB per flush with %0.1lf bytes/event\r\n",
            (totalBytesWrittenDouble/1.0e6)/fastLogger.numAioWritesCompleted,
            totalBytesWrittenDouble*1.0/numEventsProcessedDouble);

    printf("\t%0.2lf ns/event in total\r\n"
            "\t%0.2lf ns/event compressing\r\n",
            (outputTime + compressTime)*1.0e9/numEventsProcessedDouble,
            compressTime*1.0e9/numEventsProcessedDouble);

    printf("The compression ratio was %0.2lf-%0.2lfx "
            "(%lu bytes in, %lu bytes out, %lu pad bytes)\n",
                    1.0*totalBytesReadDouble/(totalBytesWrittenDouble
                                                    + padBytesWrittenDouble),
                    1.0*totalBytesReadDouble/totalBytesWrittenDouble,
                    fastLogger.totalBytesRead,
                    fastLogger.totalBytesWritten,
                    fastLogger.padBytesWritten);
}

/**
 * User API: Preallocate the thread-local data structures needed by the
 * FastLogger system for the current thread. Although optional, it is
 * recommended to invoke this function in every thread that will use the
 * FastLogger system before the first log message.
 */
void
FastLogger::preallocate()
{
    fastLogger.ensureStagingBufferAllocated();
    // I wonder if it'll be a good idea to update minFreeSpace as well since
    // the user is already willing to invoke this up front cost.
}

/**
 * Internal helper function to wait for AIO completion.
 */
void
FastLogger::waitForAIO() {
    if (hasOutstandingOperation) {
        if (aio_error(&aioCb) == EINPROGRESS) {
            const struct aiocb * const aiocb_list[] = { &aioCb };
            int err = aio_suspend(aiocb_list, 1, NULL);

            if (err != 0)
                perror("LogCompressor's Posix AIO suspend operation failed");
        }

        int err = aio_error(&aioCb);
        ssize_t ret = aio_return(&aioCb);

        if (err != 0) {
            fprintf(stderr, "LogCompressor's POSIX AIO failed with %d: %s\r\n",
                    err, strerror(err));
        } else if (ret < 0) {
            perror("LogCompressor's Posix AIO Write operation failed");
        }
        ++numAioWritesCompleted;
        hasOutstandingOperation = false;
    }
}

/**
 * Main compression thread that handles scanning through the StagingBuffers,
 * compressing log entries, and outputting a compressed log file.
 */
void
FastLogger::compressionThreadMain()
{
    //TODO(syang0) These should be abstracted away
    uint32_t lastFmtId = 0;
    uint64_t lastTimestamp = 0;

    // Index of the last StagingBuffer checked for uncompressed log messages
    size_t lastStagingBufferChecked = 0;

    // Marks when the thread wakes up. This value should be used to calculate
    // the number of cyclesAwake right before blocking/sleeping and then updated
    // to the latest rdtsc() when the thread re-awakens.
    uint64_t cyclesAwakeStart = Cycles::rdtsc();

    cycleAtThreadStart = cyclesAwakeStart;

    // Each iteration of this loop scans for uncompressed log messages in the
    // thread buffers, compresses as much as possible, and outputs it to a file.
    while (!compressionThreadShouldExit) {
        // Main buffer to put compressed log messages into
        char *out = compressingBuffer;
        char *endOfBuffer = compressingBuffer + FastLogger::OUTPUT_BUFFER_SIZE;

        {
            uint64_t start = Cycles::rdtsc();
            std::unique_lock<std::mutex> lock(bufferMutex);
            size_t i = lastStagingBufferChecked;

            // Indicates whether a compression operation failed or not due
            // to insufficient space in the outputBuffer
            bool outputBufferFull = false;

            // Indicates whether uncompressed log messages were found through
            // an iteration through all the staging buffers.
            bool workFound = false;

            // Scan through the threadBuffers looking for log messages to
            // compress while the output buffer is not full.
            while (!compressionThreadShouldExit
                        && !outputBufferFull
                        && !threadBuffers.empty()) {
                uint64_t readableBytes = 0;
                StagingBuffer *sb = threadBuffers[i];

                char *peekPosition = sb->peek(&readableBytes);

                // If there's work, unlock to perform it
                if (readableBytes > 0) {
                    uint64_t start = Cycles::rdtsc();
                    workFound = true;
                    lock.unlock();

                    uint64_t readableBytesStart = readableBytes;
                    //TODO(syang0) This should be abstracted away, me thinks.
                    while (readableBytes > 0) {
                        assert(readableBytes >= sizeof(BufferUtils::UncompressedLogEntry));

                        BufferUtils::UncompressedLogEntry *re = reinterpret_cast<
                                    BufferUtils::UncompressedLogEntry*>(peekPosition);
                        assert(re->entrySize <= readableBytes);

                        if (re->entrySize + re->argMetaBytes > endOfBuffer - out) {
                            // don't have enough space in the output to save
                            // the uncompressed form (worst case),
                            // save our place and back out
                            lastStagingBufferChecked = i;
                            outputBufferFull = true;
                            break;
                        }

                        ++eventsProcessed;

                        // Compress metadata here.
                        BufferUtils::compressMetadata(re, &out, lastTimestamp, lastFmtId);
                        lastFmtId = re->fmtId;
                        lastTimestamp = re->timestamp;

                        //TODO(syang0) This should be analogs with above
                        size_t bytesOut = compressFnArray[re->fmtId](re, out);
                        out += bytesOut;

                        readableBytes -= re->entrySize;
                        peekPosition += re->entrySize;
                        sb->consume(re->entrySize);
                    }
                    totalBytesRead += readableBytesStart - readableBytes;

                    cyclesCompressing += Cycles::rdtsc() - start;
                    lock.lock();
                } else {
                    // If there's no work, check if we're supposed to delete
                    // the stagingBuffer
                    if (sb->checkCanDelete()) {
                        threadBuffers.erase(threadBuffers.begin() + i);

                        if (i == threadBuffers.size()) {
                            if (lastStagingBufferChecked == i)
                                lastStagingBufferChecked = 0;

                            i = 0;
                        }

                        delete sb;
                        continue;
                    }
                }

                i = (i + 1) % threadBuffers.size();

                // Completed a pass through the buffers
                if (i == lastStagingBufferChecked) {
                    // If no work was found in the last pass, stop.
                    if (!workFound) {
                        break;
                    }

                    workFound = false;
                }
            }

            cyclesScanningAndCompressing += Cycles::rdtsc() - start;
        }

        // Nothing was compressed
        if (out == compressingBuffer) {
            std::unique_lock<std::mutex> lock(condMutex);

            // If a sync was requested, we should make at least 1 more
            // pass to make sure we got everything up to the sync point.
            if (syncRequested) {
                syncRequested = false;
                continue;
            }

            cyclesAwake += Cycles::rdtsc() - cyclesAwakeStart;

            hintQueueEmptied.notify_one();
            workAdded.wait_for(lock, std::chrono::microseconds(1));

            cyclesAwakeStart = Cycles::rdtsc();
            continue;
        }

        // Compressed items exist in the buffer, determine how many pad bytes
        // are needed if O_DIRECT is used and output.
        ssize_t bytesToWrite = out - compressingBuffer;
        if (FILE_PARAMS & O_DIRECT) {
            ssize_t bytesOver = bytesToWrite%512;

            if (bytesOver != 0) {
                memset(out, 0, bytesOver);
                bytesToWrite = bytesToWrite + 512 - bytesOver;
                padBytesWritten += (512 - bytesOver);
            }
        }

        uint64_t start = Cycles::rdtsc();
        if (FastLogger::USE_AIO) {
            if (hasOutstandingOperation) {
                if (aio_error(&aioCb) == EINPROGRESS) {
                    const struct aiocb * const aiocb_list[] = { &aioCb };

                    cyclesAwake += Cycles::rdtsc() - cyclesAwakeStart;
                    int err = aio_suspend(aiocb_list, 1, NULL);
                    cyclesAwakeStart = Cycles::rdtsc();

                    if (err != 0)
                        perror("LogCompressor's Posix AIO "
                                "suspend operation failed");
                }

                int err = aio_error(&aioCb);
                ssize_t ret = aio_return(&aioCb);

                if (err != 0) {
                    fprintf(stderr, "LogCompressor's POSIX AIO failed"
                            " with %d: %s\r\n", err, strerror(err));
                } else if (ret < 0) {
                    perror("LogCompressor's Posix AIO Write failed");
                }
                ++numAioWritesCompleted;
                hasOutstandingOperation = false;
            }

            aioCb.aio_fildes = outputFd;
            aioCb.aio_buf = compressingBuffer;
            aioCb.aio_nbytes = bytesToWrite;
            totalBytesWritten += bytesToWrite;

            if (aio_write(&aioCb) == -1)
                fprintf(stderr, "Error at aio_write(): %s\n", strerror(errno));

            hasOutstandingOperation = true;

            // Swap buffers
            char *tmp = compressingBuffer;
            compressingBuffer = outputDoubleBuffer;
            outputDoubleBuffer = tmp;
        } else {
            if (bytesToWrite != write(outputFd, compressingBuffer, bytesToWrite))
                perror("Error dumping log");
        }

        // TODO(syang0) Currently, the cyclesAioAndFsync metric is
        // incorrect if we use POSIX AIO since it only measures the
        // time to submit the work and (if applicable) the amount of
        // time spent waiting for a previous incomplete AIO to finish.
        // We could get a better time metric if we spawned a thread to
        // do synchronous IO on our behalf.
        cyclesAioAndFsync += (Cycles::rdtsc() - start);
    }

    if (hasOutstandingOperation) {
        uint64_t start = Cycles::rdtsc();
        // Wait for any outstanding AIO to finish
        while (aio_error(&aioCb) == EINPROGRESS);
        int err = aio_error(&aioCb);
        ssize_t ret = aio_return(&aioCb);

        if (err != 0) {
            fprintf(stderr, "LogCompressor's POSIX AIO failed with %d: %s\r\n",
                    err, strerror(err));
        } else if (ret < 0) {
            perror("LogCompressor's Posix AIO Write operation failed");
        }
        ++numAioWritesCompleted;
        hasOutstandingOperation = false;
        cyclesAioAndFsync += (Cycles::rdtsc() - start);
    }

    cycleAtThreadStart = 0;
    cyclesAwake += Cycles::rdtsc() - cyclesAwakeStart;
}

/**
 * See setLogFile
 */
void
FastLogger::setLogFile_internal(const char* filename) {
    // Check if it exists and is readable/writeable
    if (access (filename, F_OK) == 0 && access (filename, R_OK | W_OK) != 0) {
        std::string err = "Unable to read/write from file: ";
        err.append(filename);
        throw std::ios_base::failure(err);
    }

    // Try to open the file
    int newFd = open(filename, FILE_PARAMS);
    if (!newFd) {
        std::string err = "Unable to create file: ";
        err.append(filename);
        throw std::ios_base::failure(err);
    }

    // Everything seems okay, stop the background thread and change files
    sync();

     // Stop the compression thread completely
    {
        std::lock_guard<std::mutex> lock(fastLogger.condMutex);
        compressionThreadShouldExit = true;
        workAdded.notify_all();
    }

    if (compressionThread.joinable())
        compressionThread.join();

    if (outputFd > 0)
        close(outputFd);
    outputFd = newFd;

    // Relaunch thread
    compressionThreadShouldExit = false;
    compressionThread = std::thread(&FastLogger::compressionThreadMain, this);
}

/**
 * Set where the FastLogger should output its compressed log. If a previous
 * log file was specified, FastLogger will attempt to sync() the remaining log
 * entries before swapping files. For best practices, the output file shall
 * be set before the first invocation to log by the main thread as this
 * function is *not* thread safe.
 *
 * By default, the FastLogger will output to /tmp/compressedLog
 *
 * \param filename
 *      File for FastLogger to output the compress log
 *
 * \throw is_base::failure
 *      if the file cannot be opened or crated
 */
void
FastLogger::setLogFile(const char* filename)
{
    fastLogger.setLogFile_internal(filename);
}

/**
 * Blocks until the FastLogger system is able to persist to disk the
 * pending log messages that occurred before this invocation. Note that this
 * operation has similar behavior to a "non-quiescent checkpoint" in a
 * database which means log messages occurring after this point this
 * invocation may also be persisted in a multi-threaded system.
 */
void
FastLogger::sync()
{
    std::unique_lock<std::mutex> lock(fastLogger.condMutex);
    fastLogger.syncRequested = true;
    fastLogger.workAdded.notify_all();
    fastLogger.hintQueueEmptied.wait(lock);
}

/**
 * Attempt to reserve contiguous space for the producer without making it
 * visible to the consumer (See reserveProducerSpace).
 *
 * This is the slow path of reserveProducerSpace that checks for free space
 * within storage[] that involves touching variable shared with the compression
 * thread and thus causing potential cache-coherency delays.
 *
 * \param nbytes
 *      Number of contiguous bytes to reserve.
 *
 * \param blocking
 *      Test parameter that indicates that the function should
 *      return with a nullptr rather than block when there's
 *      not enough space.
 *
 * \return
 *      A pointer into storage[] that can be written to by the producer for
 *      at least nbytes.
 */
char*
FastLogger::StagingBuffer::reserveSpaceInternal(size_t nbytes, bool blocking)
{
    const char *endOfBuffer = storage + STAGING_BUFFER_SIZE;

    // There's a subtle point here, all the checks for remaining
    // space are strictly < or >, not <= or => because if we allow
    // the record and print positions to overlap, we can't tell
    // if the buffer either completely full or completely empty.
    // Doing this check here ensures that == means completely empty.

    while (minFreeSpace <= nbytes) {
        // Since readHead can be updated in a different thread, we
        // save a consistent copy of it here to do calculations on
        char *cachedReadPos = consumerPos;

        if (cachedReadPos <= producerPos) {
            // All the space between the current producerPos and the end
            // of the buffer available to us.
            minFreeSpace = endOfBuffer - producerPos;

            if (minFreeSpace > nbytes)
                return producerPos;

            // Not enough space at the end of the buffer; wrap around
            //TODO(syang0) I think a lock is needed here in case of reordering
            endOfRecordedSpace = producerPos;
            producerPos = storage;
        }

        minFreeSpace = cachedReadPos - producerPos;

        // Needed to prevent infinite loops in tests
        if (!blocking && minFreeSpace <= nbytes)
            return nullptr;
    }

    return producerPos;
}

/**
 * Peek at the data available for consumption within the stagingBuffer.
 * The consumer should also invoke consume() to release space back
 * to the producer. This can and should be done piece-wise where a
 * large peek can be consume()-ed in smaller pieces to prevent blocking
 * the producer.
 *
 * \param[out] bytesAvailable
 *      Number of bytes consumable
 * \return
 *      Pointer to the consumable space
 */
char*
FastLogger::StagingBuffer::peek(uint64_t* bytesAvailable)
{
    // Save a consistent copy of recordHead
    char *cachedRecordHead = producerPos;

    if (cachedRecordHead < consumerPos) {
        //TODO(syang0) LocK? See reserveSpaceInternal
        *bytesAvailable = endOfRecordedSpace - consumerPos;

        if (*bytesAvailable > 0)
            return consumerPos;

        // Roll over
        consumerPos = storage;
    }

    *bytesAvailable = cachedRecordHead - consumerPos;
    return consumerPos;
}

} // PerfUtils