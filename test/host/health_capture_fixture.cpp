#include "../mocks/HealthCaptureFixture.h"
#include <fstream>

// Generates synthetic records for the Python transport-to-production replay test.
int main(int argc,char** argv) {
  if(argc!=3) return 2;
  HealthCaptureFixture f(argv[2]);
  f.now=UINT32_MAX-2000; f.calibration.bias[0]=.125f; f.calibration.samples=700;
  f.begin(); f.hold(1200); f.sample(.3f); f.sample(3,0,300); f.hold(31000,1.4f,0,40);
  f.hold(11000,1); f.discontinuity(); f.hold(1200); f.sample(.3f); f.hold(6200);
  f.end(); std::ofstream output(argv[1],std::ios::binary);
  output.write(reinterpret_cast<const char*>(f.bytes.data()),f.bytes.size()); return output ? 0 : 1;
}
