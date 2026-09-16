#include "EventLoop.h"

#define PORT 8080

int main()
{
  EventLoop loop(PORT);
  loop.run();
  return 0;
}
