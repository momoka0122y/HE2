// g++ -std=c++14 2HE.cpp -lpthread
#include <arpa/inet.h>
#include <condition_variable>
#include <errno.h>
#include <iostream>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::literals::chrono_literals;

// from RFC8305 section8 Summary of Configurable Values
// https://datatracker.ietf.org/doc/html/rfc8305#section-8 

// The time to wait for a AAAA response after receiving an A response.
#define RESOLUTION_DELAY 50ms


/*
        State change diagram
        WaitingBoth : waiting A or AAAA after requesting A and AAAA
        WaitingBoth             -> SendIPv6 : if AAAA is received
        WaitingBoth             -> WaitingAAAA : if A is received
        WaitingAAAA             -> SendBoth : if AAAA is received
        WaitingAAAA             -> SendIPv4WaitingAAAA : if timeout
        SendIPv6                -> SendBoth : if A is received
        SendIPv4WaitingAAAA     -> SendBoth : if AAAA is received
                                -> Connected
*/

enum class State {
  WaitingBoth,
  WaitingAAAA,
  SendIPv4WaitingAAAA,
  SendIPv6,
  SendBoth,
  Connected,
};


struct Notification {
  std::mutex m;
  State status = State::WaitingBoth;
  struct addrinfo *addrlist;
  struct addrinfo *addrlist_IPv4;
  struct addrinfo *addrlist_IPv6;
  std::condition_variable cv;
};


void getaddr(const char *hostname, struct addrinfo **addrlist, const int isIPv4) {
  struct addrinfo hints;
  char host[256];

  memset(&hints, 0, sizeof(struct addrinfo));
  if (isIPv4){
    hints.ai_family = AF_INET;
  } else{
    hints.ai_family = AF_INET6;
  }
  hints.ai_socktype = SOCK_STREAM;

  int ret = getaddrinfo(hostname, "80", &hints, addrlist);

  struct addrinfo *tmp;
  char hostt[256];
  for (tmp = *addrlist; tmp != NULL; tmp = tmp->ai_next) {
    getnameinfo(tmp->ai_addr, tmp->ai_addrlen, hostt, sizeof(host), NULL, 0,
                NI_NUMERICHOST);
    puts(hostt);
  }

  if (ret != 0) {
    fprintf(stderr, "getaddrinfo IPv%d: %s\n", 6-2*isIPv4, gai_strerror(ret));
    // exit(EXIT_FAILURE);
    return;
  }
}


// WaitingBoth             -> WaitingAAAA : if A is received
// SendIPv6                -> SendBoth : if A is received 
void ipv4proc(Notification &notif, const std::chrono::milliseconds &waitTime,
              const char *hostname) {
  puts("request A");
  std::this_thread::sleep_for(waitTime);
  {
    std::lock_guard<std::mutex> lk(notif.m);
    if (notif.status == State::WaitingBoth ||
        notif.status == State::SendIPv6) {
      getaddr(hostname, &notif.addrlist_IPv4, 1);
      // addIPv4list(notif.addrlist, notif.addrlist_IPv4);
      if (notif.status == State::WaitingBoth) {
        notif.status = State::WaitingAAAA;
      }else if ( notif.status == State::SendIPv6 ){
        notif.status = State::SendBoth;
      }
      
    }
  }
  puts("receive A");
  notif.cv.notify_one();
}


// WaitingBoth             -> SendIPv6 : if AAAA is received
// WaitingAAAA             -> SendBoth : if AAAA is received
// SendIPv4WaitingAAAA     -> SendBoth : if AAAA is received

void ipv6proc(Notification &notif, const std::chrono::milliseconds &waitTime,
              const char *hostname) {
  puts("request AAAA");
  std::this_thread::sleep_for(waitTime);
  {
    std::lock_guard<std::mutex> lk(notif.m);
    if (notif.status == State::WaitingBoth ||
        notif.status == State::WaitingAAAA ||
        notif.status == State::SendIPv4WaitingAAAA) {
      getaddr(hostname, &notif.addrlist_IPv6, 0);
      notif.status = State::SendIPv6;

      if (notif.status == State::WaitingBoth ) {
        notif.status = State::SendIPv6;
      }else if ( notif.status == State::WaitingAAAA ||
        notif.status == State::SendIPv4WaitingAAAA ){
        notif.status = State::SendBoth;
      }
      }


  }
  puts("receive AAAA");
  notif.cv.notify_one();


}



void happyEyeball2(const std::chrono::milliseconds waitTime1,
                   const std::chrono::milliseconds waitTime2,const  char *hostname) {
  std::cout << "test happyEyeball2 time:" << waitTime1.count() << "ms "
            << waitTime2.count() << "ms" << std::endl;
  Notification notif;
  // request A and AAAA
  std::thread t1(ipv4proc, std::ref(notif), waitTime1, hostname);
  std::thread t2(ipv6proc, std::ref(notif), waitTime2, hostname);

  // waiting both
  {
    std::unique_lock<std::mutex> lk(notif.m);
    notif.cv.wait(lk, [&] { return notif.status != State::WaitingBoth; });
  }
  // A is received so wait 50ms
    // WaitingAAAA             -> SendBoth : if AAAA is received
    // WaitingAAAA             -> SendIPv4WaitingAAAA : if timeout
  if (notif.status == State::WaitingAAAA) {
    const auto waitIpv6Time = 50ms;
    std::cout << "wait ipv6 " << waitIpv6Time.count() << "ms" << std::endl;
    {
      std::unique_lock<std::mutex> lk(notif.m);
      if (notif.cv.wait_for(lk, waitIpv6Time,
                            [&] { return notif.status == State::SendIPv6; })) {
        puts("receive ipv6");
      } else {
        puts("timeout");
        notif.status = State::SendIPv4WaitingAAAA;
      }
    }
  }
  struct addrinfo *tmp;
  char host[256];
  int fd = -1;
  switch (notif.status) {
  case State::SendIPv4WaitingAAAA:
    puts("ipv4 syn");
    for (tmp = notif.addrlist_IPv4; tmp != NULL; tmp = tmp->ai_next) {
      getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0,
                  NI_NUMERICHOST);
      puts(host);
    }
    for (tmp = notif.addrlist_IPv4; tmp != NULL; tmp = tmp->ai_next) {
      printf("start connect IPv4 \n");
      fd = socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
      if (fd == -1) {
        continue;
      }
      if (connect(fd, tmp->ai_addr, (socklen_t)tmp->ai_addrlen) != 0) {
        fprintf(stderr, "connect: %s\n", strerror(errno));
      } else {
        printf("connected to ");
        getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0,
                  NI_NUMERICHOST);
        puts(host);
      }
      break;
    }
    break;
  case State::SendIPv6:
    puts("ipv6 syn");
    for (tmp = notif.addrlist_IPv6; tmp != NULL; tmp = tmp->ai_next) {
      getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0,
                  NI_NUMERICHOST);
      puts(host);
    }
    for (tmp = notif.addrlist_IPv6; tmp != NULL; tmp = tmp->ai_next) {
      printf("start connect IP6 \n");
      fd = socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
      if (fd == -1) {
        continue;
      }
      if (connect(fd, tmp->ai_addr, (socklen_t)tmp->ai_addrlen) != 0) {
        fprintf(stderr, "connect: %s\n", strerror(errno));
      } else {
        printf("connected to ");
        getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0,
                  NI_NUMERICHOST);
        puts(host);
      }

      break;
    }
    break;
  default:
    puts("ERR");
    break;
  }

  t1.join();
  t2.join();
  puts("------------");
}


int main(int argc, char *argv[]) {
  const char *hostname;
  if (argc >= 2) {
    hostname = argv[1];
  } else {
    hostname = "momoka.hongo.wide.ad.jp";
  }
  puts("case 1. AAAA is fast so send ipv6 syn");
  happyEyeball2(2000ms, 1000ms, hostname);
  puts("case 2. A is fast so send ipv4 syn");
  happyEyeball2(1000ms, 2000ms, hostname);
  puts("case 3. A is a little fast so wait AAAA and send ipv6 syn");
  happyEyeball2(1000ms, 1010ms, hostname);
}
