
import numpy as np
import time

from cereal import messaging
from openpilot.common.realtime import Ratekeeper
from openpilot.common.retry import retry
from openpilot.common.swaglog import cloudlog
from openpilot.system.micd import SAMPLE_BUFFER, SAMPLE_RATE

class Soundd:
  def __init__(self):
    self.raw_sample = np.ndarray(SAMPLE_BUFFER, dtype=np.int16)
    self.init = False
    self.frame_index_last = 0

  def callback(self, data_out: np.ndarray, frames: int, _, status) -> None:
    if status:
        cloudlog.warning(f"soundd stream over/underflow: {status}")
    if self.init:
        data_out[:frames, 0] = self.raw_sample.copy()


  @retry(attempts=7, delay=3)
  def get_stream(self, sd):
    # reload sounddevice to reinitialize portaudio
    sd._terminate()
    sd._initialize()

    return sd.OutputStream(channels=1, samplerate=SAMPLE_RATE, callback=self.callback, blocksize=SAMPLE_BUFFER, dtype="int16")

  def soundd_thread(self):
    # sounddevice must be imported after forking processes
    import sounddevice as sd

    self.sm = messaging.SubMaster(['microphoneRaw'])

    with self.get_stream(sd) as stream:
      #rk = Ratekeeper(20)
      print(f"soundd stream started: {stream.samplerate=} {stream.channels=} {stream.dtype=} {stream.device=}, {stream.blocksize=}")

      while True:
        self.sm.update(0)
        if self.sm.updated['microphoneRaw']:
            self.init = True
        if self.init:
          self.frame_index = self.sm['microphoneRaw'].frameIndex
          #if self.frame_index != self.frame_index_last:
          self.raw_sample = np.frombuffer(self.sm['microphoneRaw'].rawSample, dtype=np.int16)
          if not (self.frame_index_last == self.frame_index or 
            self.frame_index - self.frame_index_last == SAMPLE_BUFFER):
            print(f'skipped {(self.frame_index - self.frame_index_last)//SAMPLE_BUFFER} sample(s)')
          self.frame_index_last = self.frame_index
          print(self.frame_index)
        assert stream.active

def main():
  s = Soundd()
  s.soundd_thread()

if __name__ == "__main__":
  main()
