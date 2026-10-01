#pragma once
// Reuse only the stream fake, without exposing AES/SHA/Mesh test doubles.
#define Stream TransportBaseStream
#include "../mocks/Stream.h"
#undef Stream
class Stream : public TransportBaseStream {
public:
  size_t println() { return write("\r\n"); }
};
