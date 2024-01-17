import asyncio
import numpy as np
import sounddevice as sd
from cereal import messaging

# Global audio queue
audio_queue = asyncio.Queue()


async def record_to_queue(**kwargs):
    loop = asyncio.get_event_loop()

    def callback(indata, frame_count, time_info, status):
        if status:
            print(status)
        # Put audio frames into the queue
        loop.call_soon_threadsafe(audio_queue.put_nowait, indata.copy())

    stream = sd.InputStream(callback=callback, **kwargs)
    with stream:
        # Run indefinitely
        while True:
            await asyncio.sleep(0.1)  # Small sleep to yield control

async def play_from_queue(**kwargs):
    loop = asyncio.get_event_loop()

    def callback(outdata, frame_count, time_info, status):
        if status:
            print(status)
        try:
            # Get audio frames from the queue
            indata = audio_queue.get_nowait()

            # Determine the number of frames to copy
            valid_frames = min(len(indata), len(outdata))
            outdata[:valid_frames] = indata[:valid_frames]

            # Fill the rest of the buffer with zeros if necessary
            outdata[valid_frames:] = 0
        except asyncio.QueueEmpty:
            # Fill with zeros if queue is empty
            outdata.fill(0)
            print("empty")

    stream = sd.OutputStream(callback=callback, **kwargs)
    with stream:
        # Run indefinitely
        while True:
            await asyncio.sleep(0.1)  # Small sleep to yield control

async def main(channels=1, dtype='float32', **kwargs):
    record_task = asyncio.create_task(record_to_queue(channels=channels, dtype=dtype, **kwargs))
    play_task = asyncio.create_task(play_from_queue(channels=channels, dtype=dtype, **kwargs))

    await asyncio.gather(record_task, play_task)

if __name__ == "__main__":
    try:
        asyncio.run(main(blocksize=2048, samplerate=2048*8))
    except KeyboardInterrupt:
        print('\nInterrupted by user')
