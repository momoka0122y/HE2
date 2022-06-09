// g++ -std=c++14 2HE.cpp -lpthread -g
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
#define RESOLUTION_DELAY            50ms

// The time to wait between connection attempts
#define CONNECTION_ATTEMPT_DELAY   250ms


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


void connect_to(Notification &notif) {
  printf("connect to address\n");
  {
    std::unique_lock<std::mutex> lk(notif.m);
    notif.status = State::Connected;
    notif.cv.notify_one();
  }
  printf("connected\n");
  return;


  // struct addrinfo *tmp;
  // char host[256];
  // int fd = -1;
  // puts("ipv6 syn");
  // for (tmp = notif.addrlist_IPv6; tmp != NULL; tmp = tmp->ai_next) {
  //   getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0,
  //               NI_NUMERICHOST);
  //   puts(host);
  // }
  // for (tmp = notif.addrlist_IPv6; tmp != NULL; tmp = tmp->ai_next) {
  //   printf("start connect IP6 \n");
  //   fd = socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
  //   if (fd == -1) {
  //     continue;
  //   }
  //   if (connect(fd, tmp->ai_addr, (socklen_t)tmp->ai_addrlen) != 0) {
  //     fprintf(stderr, "connect: %s\n", strerror(errno));
  //   } else {
  //     printf("connected to ");
  //     getnameinfo(tmp->ai_addr, tmp->ai_addrlen, host, sizeof(host), NULL, 0,
  //               NI_NUMERICHOST);
  //     puts(host);
  //   }
}


// WaitingBoth             -> WaitingAAAA : if A is received
// SendIPv6                -> SendBoth : if A is received 
void ipv4proc(Notification &notif, const std::chrono::milliseconds &waitTime,
              const char *hostname) {
  puts("request A");
  std::this_thread::sleep_for(waitTime);
  {
    std::lock_guard<std::mutex> lk(notif.m);
    if (notif.status != State::WaitingBoth &&
        notif.status != State::SendIPv6) {
      return;
    } 
    
    getaddr(hostname, &notif.addrlist_IPv4, 1);
    // addIPv4list(notif.addrlist, notif.addrlist_IPv4);
    if (notif.status == State::WaitingBoth) {
      notif.status = State::WaitingAAAA;
    }else if ( notif.status == State::SendIPv6 ){
      notif.status = State::SendBoth;
    }

  }
  puts("receive A");
  notif.cv.notify_all();





  //  WaitingAAAA ならまつ
  // {
  //   std::unique_lock<std::mutex> lk(notif.m);
  //   if (notif.status == State::WaitingAAAA) {
  //     printf("waiting AAAA\n");
  //     notif.cv.wait(lk, [&] { return notif.status == State::SendIPv4WaitingAAAA 
  //                                 || notif.status == State::SendBoth; });
  //     printf("waiting AAAA finished\n");
  //   }
  // }


  {
    std::unique_lock<std::mutex> lk(notif.m);
    if (notif.status == State::WaitingAAAA) {
      std::cout << "wait ipv6 " << RESOLUTION_DELAY.count() << "ms" << std::endl;
      if (notif.cv.wait_for(lk, RESOLUTION_DELAY,
                            [&] { return notif.status == State::SendBoth || notif.status == State::Connected; })) {
        switch (notif.status){
          case State::SendBoth:
            puts("receive ipv6");
          case State::Connected:
            return;
        }
        
      } else {
        puts("timeout");
        notif.status = State::SendIPv4WaitingAAAA;
        
      }
      notif.cv.notify_all();
    }
  }


  // SendIPv4WaitingAAAA, SendBoth のときに送る。
//  lock逆！！！

  {
    std::unique_lock<std::mutex> lk(notif.m);
    if (notif.status == State::SendIPv4WaitingAAAA || notif.status == State::SendBoth) {
      printf("162\n");
      connect_to(notif);
      return;
    }
  }



  {
    std::unique_lock<std::mutex> lk(notif.m);
    if (notif.status != State::Connected){
      if (notif.cv.wait_for(lk, CONNECTION_ATTEMPT_DELAY,
                            [&] { return notif.status == State::Connected; })) {
        puts("connected ipv6 while waiting for CONNECTION_ATTEMPT_DELAY");
        return;
      } else {
        puts("CONNECTION_ATTEMPT_DELAY timeout");
        printf("179\n");
        connect_to(notif);
        return;
      }
    }


  }

}





// WaitingBoth             -> SendIPv6 : if AAAA is received
// WaitingAAAA             -> SendBoth : if AAAA is received
// SendIPv4WaitingAAAA     -> SendBoth : if AAAA is received

void ipv6proc(Notification &notif, const std::chrono::milliseconds &waitTime,
              const char *hostname) {



  {
    std::unique_lock<std::mutex> lk(notif.m);

      puts("request AAAA");
      if ( notif.status != State::Connected && notif.cv.wait_for(lk, waitTime,
                            [&] { return notif.status == State::Connected; })) {
        puts("ipv4 connected while waiting for AAAA");
        return;
      } else {
        if (notif.status == State::WaitingBoth ||
            notif.status == State::WaitingAAAA ||
            notif.status == State::SendIPv4WaitingAAAA) {
          getaddr(hostname, &notif.addrlist_IPv6, 0);
          // notif.status = State::SendIPv6;

          switch(notif.status){
            case State::WaitingBoth:
              notif.status = State::SendIPv6;
            case State::WaitingAAAA:
            case State::SendIPv4WaitingAAAA:
              notif.status = State::SendBoth;
          }
        }
        
      }

  }



  puts("receive AAAA");
  notif.cv.notify_one();



  // 今 SendIPv6, SendBoth のどちらか。
  // もしSendIPv6なら
  // SendIPv6  -> SendBoth : if A is received

  // すぐにconnect初めていい

  // ひとつ目にconnect
  printf("ipv6 goto connect\n");
  connect_to(notif);
  return;
  // 250ms まつ、　もし SendBoth ならさらに 250msまつ。

  // 次にconnect


}



void happyEyeball2(const std::chrono::milliseconds waitTime1,
                   const std::chrono::milliseconds waitTime2,const  char *hostname) {
  std::cout << "test happyEyeball2 time:" << waitTime1.count() << "ms "
            << waitTime2.count() << "ms" << std::endl;
  Notification notif;
  // request A and AAAA
  std::thread t1(ipv4proc, std::ref(notif), waitTime1, hostname);
  std::thread t2(ipv6proc, std::ref(notif), waitTime2, hostname);

  // // waiting both
  // {
  //   std::unique_lock<std::mutex> lk(notif.m);
  //   notif.cv.wait(lk, [&] { return notif.status != State::WaitingBoth; });
  // }
  // A is received so wait 50ms
    // WaitingAAAA             -> SendBoth : if AAAA is received
    // WaitingAAAA             -> SendIPv4WaitingAAAA : if timeout
  // {
  //   std::unique_lock<std::mutex> lk(notif.m);
  //   if (notif.status == State::WaitingAAAA) {
  //     std::cout << "wait ipv6 " << RESOLUTION_DELAY.count() << "ms" << std::endl;
  //     if (notif.cv.wait_for(lk, RESOLUTION_DELAY,
  //                           [&] { return notif.status == State::SendBoth; })) {
  //       puts("receive ipv6");
  //     } else {
  //       puts("timeout");
  //       notif.status = State::SendIPv4WaitingAAAA;
  //       notif.cv.notify_all();
  //     }
      
  //   }
  // }




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
  // puts("case 1. AAAA is fast so send ipv6 syn");
  // happyEyeball2(2000ms, 1000ms, hostname);
  puts("case 2. A is fast so send ipv4 syn");
  happyEyeball2(1000ms, 2000ms, hostname);
  puts("case 3. A is a little fast so wait AAAA and send ipv6 syn");
  happyEyeball2(1000ms, 1010ms, hostname);
}
