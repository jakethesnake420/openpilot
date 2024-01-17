import numpy as np
from cereal import messaging

class Soundd:
    def __init__(self, duration, sample_rate):
        self.sample_rate = sample_rate
        self.max_samples = duration * sample_rate
        self.buffer = np.empty((self.max_samples), dtype=np.float32)
        self.current_index = 0
        self.sm = messaging.SubMaster(['microphone'])
    
    def accumulate_samples(self):
        while self.current_index < self.max_samples:
            self.sm.update(0)
            if self.sm.updated['microphone']:
                
                new_samples = np.frombuffer(self.sm['microphone'].rawSample[0], dtype=np.float32)
                samples_to_copy = min(len(new_samples), self.max_samples - self.current_index)
                self.buffer[self.current_index:self.current_index + samples_to_copy] = new_samples[:samples_to_copy]
                self.current_index += samples_to_copy
                print(self.current_index)
                if self.current_index >= self.max_samples:
                    break
                
        return self.buffer[:self.current_index]
    
s = Soundd(2, 2048*8)
s.accumulate_samples()