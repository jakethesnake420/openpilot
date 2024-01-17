#!/usr/bin/env python3
import numpy as np

from cereal import messaging
from openpilot.common.realtime import Ratekeeper
from openpilot.common.retry import retry
from openpilot.common.swaglog import cloudlog
from queue import Queue
import threading
import copy
import time

RATE = 10
FFT_SAMPLES = 2048
REFERENCE_SPL = 2e-5  # newtons/m^2

SAMPLE_RATE = 16000
SAMPLE_BUFFER = 1280 # (approx 100ms)
DOWN_SAMPLE_FACTOR = 4
DOWN_SAMPLE_RATE = SAMPLE_RATE#int(SAMPLE_RATE/DOWN_SAMPLE_FACTOR)
DOWN_SAMPLE_BUFFER = SAMPLE_BUFFER#int(SAMPLE_BUFFER/DOWN_SAMPLE_FACTOR)

def simple_low_pass_filter(signal, factor):
    """
    A very basic low-pass filter: simple moving average.
    """
    return np.convolve(signal, np.ones(factor)/factor, mode='valid')

def downsample_audio(audio, factor):
    """
    Downsample the audio by the given factor.
    """
    # Apply low-pass filter
    filtered_audio = simple_low_pass_filter(audio, factor)

    # Decimate
    downsampled_audio = filtered_audio[::factor]

    return downsampled_audio

def calculate_spl(measurements):
  # https://www.engineeringtoolbox.com/sound-pressure-d_711.html
  sound_pressure = np.sqrt(np.mean(measurements ** 2))  # RMS of amplitudes
  if sound_pressure > 0:
    sound_pressure_level = 20 * np.log10(sound_pressure / REFERENCE_SPL)  # dB
  else:
    sound_pressure_level = 0
  return sound_pressure, sound_pressure_level


def apply_a_weighting(measurements: np.ndarray) -> np.ndarray:
  # Generate a Hanning window of the same length as the audio measurements
  measurements_windowed = measurements * np.hanning(len(measurements))

  # Calculate the frequency axis for the signal
  freqs = np.fft.fftfreq(measurements_windowed.size, d=1 / SAMPLE_RATE)

  # Calculate the A-weighting filter
  # https://en.wikipedia.org/wiki/A-weighting
  A = 12194 ** 2 * freqs ** 4 / ((freqs ** 2 + 20.6 ** 2) * (freqs ** 2 + 12194 ** 2) * np.sqrt((freqs ** 2 + 107.7 ** 2) * (freqs ** 2 + 737.9 ** 2)))
  A /= np.max(A)  # Normalize the filter

  # Apply the A-weighting filter to the signal
  return np.abs(np.fft.ifft(np.fft.fft(measurements_windowed) * A))


class Mic:
  def __init__(self):
    self.rk = Ratekeeper(RATE)
    self.pm = messaging.PubMaster(['microphoneRaw'])

    self.measurements = np.empty(0)

    self.sound_pressure = 0
    self.sound_pressure_weighted = 0
    self.sound_pressure_level_weighted = 0
    self.raw_sample = np.ndarray(SAMPLE_BUFFER, dtype=np.int16)
    self.raw_sample_last = np.ndarray(SAMPLE_BUFFER, dtype=np.float32)
    self.frame_index = 0
    self.frame_index_last = 0
    self.audio_queue = Queue()
    self.data_ready_event = threading.Event()
    
  def update(self):
    st = time.time()
    # Send the audio frame
    msg = messaging.new_message('microphoneRaw', valid=True)
    self.data_ready_event.wait()
    #data = np.int16(self.raw_sample * 32767)
    #downsampled_audio = downsample_audio(self.raw_sample, DOWN_SAMPLE_FACTOR)
    int16_sample = np.int16(self.raw_sample * 32767)
    msg.microphoneRaw.rawSample = int16_sample.tobytes()
    print(len(msg.microphoneRaw.rawSample))
    #msg.microphoneRaw.rawSample = np.int16(self.raw_sample * 32767).tobytes()
    print(f'{self.frame_index_last=}, {self.frame_index=}')
    if not (self.frame_index_last == self.frame_index or 
            self.frame_index - self.frame_index_last == SAMPLE_BUFFER):
      print(f'skipped {(self.frame_index - self.frame_index_last)//SAMPLE_BUFFER-1} sample(s)')

    self.frame_index_last = self.frame_index

    
    msg.microphoneRaw.frameIndex = self.frame_index
    self.pm.send('microphoneRaw', msg)
    
    
    self.data_ready_event.clear()
    
  def callback(self, indata, frames, _, status):
    
    """
    Using amplitude measurements, calculate an uncalibrated sound pressure and sound pressure level.
    Then apply A-weighting to the raw amplitudes and run the same calculations again.

    Logged A-weighted equivalents are rough approximations of the human-perceived loudness.
    """
    if status:
      cloudlog.warning(f"soundd stream over/underflow: {status}")
    self.frame_index += frames

    self.raw_sample = indata[:,0].copy()
    self.data_ready_event.set()
    


  @retry(attempts=7, delay=3)
  def get_stream(self, sd):
    # reload sounddevice to reinitialize portaudio
    sd._terminate()
    sd._initialize()
    return sd.InputStream(channels=1, samplerate=SAMPLE_RATE, callback=self.callback, blocksize=SAMPLE_BUFFER, dtype="float32")

  def micd_thread(self):
    # sounddevice must be imported after forking processes
    import sounddevice as sd
    print(sd.query_devices())
    with self.get_stream(sd) as stream:
      print(f"micd stream started: {stream.samplerate=} {stream.channels=} {stream.dtype=} {stream.device=}, {stream.blocksize=}")
      while True:
        self.update()


def main():
  mic = Mic()
  mic.micd_thread()


if __name__ == "__main__":
  main()
