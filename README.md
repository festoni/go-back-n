# go-back-n
Go-back-N TCP protocol for Linux machines.

Sends and receives packets between client and server using maximum window size of 2. All corrupt, dropped, or out of order packet are considered.

usage (Linux only): 
```
./sender <hostname> <port> <filename>
./receiver <port> <filename>
```