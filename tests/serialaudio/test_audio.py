"""Exercise production serialaudio.cpp using deterministic GPIO/I2C stubs."""
import ctypes as c
from pathlib import Path
import struct, sys, unittest
D=c.CDLL(str(Path(sys.argv.pop(1)).resolve()))
D.command.argtypes=[c.c_uint,c.c_char_p,c.c_uint]
class AudioTests(unittest.TestCase):
 def setUp(self):D.init()
 def tearDown(self):self.assertEqual(D.hardware(7),0)
 def send(self,op,data=b''):D.command(op,data,len(data))
 def pcm(self,pos,data):self.send(0x58,struct.pack('<I',pos)+data)
 def event(self,pos,kind,data):self.send(0x59,struct.pack('<IB',pos,kind)+data)
 def start(self):self.send(0x5a,struct.pack('<IH',8000000,512))
 def status(self):
  b=c.create_string_buffer(40);D.status(b);return struct.unpack('<10I',b.raw)
 def test_normal_end_and_silence_stay_running(self):
  self.pcm(0,b'\x12\x34\x56');self.event(3,3,b'\x08');self.start();D.tick(6)
  self.assertEqual([D.output(i) for i in range(6)],[0x12,0x34,0x56,8,8,8])
  self.assertEqual(self.status()[:6],(3,3,0,0,0,0));self.assertEqual(self.status()[7],3)
  self.assertEqual(D.hardware(3),1);self.assertEqual(D.hardware(5),0)
 def test_missing_frame_latches_fault_no_resume(self):
  self.pcm(0,b'abc');self.pcm(4,b'd');self.assertEqual(self.status()[5],1)
  self.start();self.assertEqual(D.hardware(3),0)
  self.pcm(3,b'd');self.assertEqual(self.status()[0],3)
 def test_underflow_mutes_and_stays_faulted(self):
  self.pcm(0,b'a');self.start();D.tick(2);D.service()
  self.assertEqual(self.status()[3],1);self.assertEqual(self.status()[7],69)
  self.assertEqual(D.hardware(5),1)
  self.pcm(1,b'b');D.tick(3);self.assertEqual(self.status()[1],1)
 def test_wrap_and_refill_order(self):
  all_bytes=bytes(i%251 for i in range(10000));sent=0;played=0
  while sent<512:
   n=min(252,512-sent);self.pcm(sent,all_bytes[sent:sent+n]);sent+=n
  self.start()
  while sent<len(all_bytes):
   D.tick(120);played+=120
   n=min(120,len(all_bytes)-sent);self.pcm(sent,all_bytes[sent:sent+n]);sent+=n
  self.event(sent,3,b'\x80');D.tick(sent-played)
  self.assertEqual(bytes(D.output(i) for i in range(sent)),all_bytes)
  self.assertEqual(self.status()[3:6],(0,0,0))
 def test_overflow_rejects_whole_chunk(self):
  pos=0
  while pos<4000:
   n=min(252,4000-pos);self.pcm(pos,b'x'*n);pos+=n
  self.pcm(pos,b'y'*200)
  self.assertEqual(self.status()[0],4000);self.assertEqual(self.status()[4],1)
 def test_controls_run_at_byte_position_and_clock_4mhz(self):
  self.pcm(0,b'a'*30)
  self.event(5,0,b'\x08\x78');self.event(5,1,struct.pack('<IH',4000000,768));self.event(5,2,b'\x01')
  self.start();D.tick(4);D.service();self.assertEqual(D.hardware(1),0)
  D.tick(1);D.service()
  self.assertEqual(D.hardware(1),1);self.assertEqual(D.hardware(2),768)
  self.assertEqual(D.hardware(6),4000000);self.assertEqual(D.hardware(4),1)
  self.assertEqual(self.status()[8:],(0,0))
 def test_reset_fences_old_data_and_events(self):
  self.pcm(0,b'ab');self.event(1,0,b'\x08\x78');self.start();D.tick(1);D.reset();D.tick(3);D.service()
  self.assertEqual(D.hardware(0),1);self.assertEqual(D.hardware(1),0)
  self.assertEqual(self.status(),(0,)*10)
  self.pcm(0,b'c');self.start();D.tick(1);self.assertEqual(D.output(1),ord('c'))
 def test_transport_loss_mutes(self):
  self.pcm(0,b'abc');self.start();D.lost();D.service()
  self.assertEqual(D.hardware(5),1);self.assertEqual(self.status()[7],13)
 def test_bad_lengths_and_controls(self):
  for op,payload in [(0x58,b''),(0x59,b''),(0x59,struct.pack('<IBB',0,2,4)),
                     (0x5a,struct.pack('<IH',4000000,123))]:
   D.init();self.send(op,payload);self.assertEqual(self.status()[5],1)
 def test_event_order_overflow_and_lateness(self):
  self.pcm(0,b'a'*50);self.event(10,0,b'\x08\x78');self.event(9,2,b'\0')
  self.assertEqual(self.status()[5],1)
  D.init();self.pcm(0,b'a'*50);self.event(5,0,b'\x08\x78');self.start();D.tick(8);D.service()
  self.assertEqual(self.status()[8:],(1,3))
  D.init()
  for i in range(1025):self.event(0,0,b'\x08\x78')
  self.assertEqual(self.status()[4],1)
if __name__=='__main__':unittest.main()
