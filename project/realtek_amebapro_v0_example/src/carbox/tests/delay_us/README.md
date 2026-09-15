# USB DelayUs host tests

From this directory:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=undefined -I. test.cpp -o /tmp/rt8715-delay-us-test
/tmp/rt8715-delay-us-test
```

Compiles the production delay implementation with simulated CMSIS registers.
Checks 300/400 MHz, fractional-MHz rounding, multiplication overflow, counter
rollover, preemption, zero/long delays, and unavailable/stalled DWT fallback.
These tests do not measure physical timing, peripheral latency, or USB behavior.
