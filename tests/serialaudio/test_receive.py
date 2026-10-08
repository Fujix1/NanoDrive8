"""Production SerialMan + Protocol + SerialAudio, with simulated USB and chips."""
import ctypes as c, struct, sys, unittest, binascii
from pathlib import Path
D=c.CDLL(str(Path(sys.argv.pop(1)).resolve()))
D.queueBytes.argtypes=[c.c_char_p,c.c_uint]
D.command.argtypes=[c.c_uint,c.c_char_p,c.c_uint]
def frame(op,payload):
 raw=struct.pack('<2sBBHH',b'ND',1,op,1,len(payload))+payload
 raw+=struct.pack('<H',binascii.crc_hqx(raw,0xffff))
 out=bytearray([0]);pos=0;code=1
 for byte in raw:
  if not byte:out[pos]=code;pos=len(out);out.append(0);code=1
  else:
   out.append(byte);code+=1
   if code==255:out[pos]=code;pos=len(out);out.append(0);code=1
 out[pos]=code
 return b'\0'+out+b'\0'
class ReceiveTests(unittest.TestCase):
 def setUp(self):D.receiveInit()
 def tearDown(self):self.assertEqual(D.hardware(7),0)
 def packet(self,op,payload):
  b=frame(op,payload);D.queueBytes(b,len(b));D.receivePass(1)
 def status(self):
  b=c.create_string_buffer(40);D.status(b);return struct.unpack('<10I',b.raw)
 def prepare(self):
  self.packet(0x58,struct.pack('<I',0)+b'a'*100)
  self.packet(0x5a,struct.pack('<IH',8000000,512))
 def test_sof_flaps_preserve_running_audio_and_bytes(self):
  self.prepare();D.tick(1)
  for index in range(20):
   D.sof(index%2)
   self.packet(0x58,struct.pack('<I',100+index)+b'b')
   D.tick(1)
  self.assertEqual(self.status()[0:2],(120,21))
  self.assertEqual(self.status()[7],1)
  self.assertEqual(D.receiveValue(2),0)
  self.assertGreater(D.receiveValue(5),0)
 def test_sof_flap_preserves_partial_frame(self):
  self.prepare();b=frame(0x58,struct.pack('<I',100)+b'new')
  D.queueBytes(b[:8],8);D.receivePass(1)
  D.sof(0);D.queueBytes(b[8:],len(b)-8);D.receivePass(1)
  self.assertEqual(self.status()[0],103);self.assertEqual(self.status()[7],1)
 def test_actual_bus_reset_faults_and_discards_old_rx(self):
  self.prepare();b=frame(0x58,struct.pack('<I',100)+b'old');D.queueBytes(b,len(b))
  D.busReset();D.receivePass(1)
  self.assertEqual(self.status()[7],13);self.assertEqual(self.status()[0],100)
  self.assertEqual(D.receiveValue(3),1);self.assertGreater(D.receiveValue(2),0)
 def test_rx_overflow_is_reported_separately_from_audio_overflow(self):
  self.prepare();b=b'x'*8704;D.queueBytes(b,len(b))
  for _ in range(17):D.receivePass(0)
  D.receivePass(1)
  self.assertEqual(self.status()[7],21)
  self.assertEqual(self.status()[3:6],(0,0,0));self.assertEqual(D.receiveValue(1),1)
 def test_explicit_discard_and_reset_recovery(self):
  self.prepare();D.discard();self.assertEqual(self.status()[7],37)
  self.packet(0x00,b'');self.assertEqual(self.status(),(0,)*10)
  self.prepare();self.assertEqual(self.status()[7],1)
if __name__=='__main__':unittest.main()
