# 예시 문서

# Q12. 소켓 연결 수립·종료와 포트 식별

> 네트워크 | TCP Socket | 기본

## 질문

1. socket close할 때 3-way handshake를 하나?
2. 만약 인터넷이 예상치 못하게 끊겨서 3-way handshake를 못하면 어떻게 되나? 클라이언트측, 서버측 둘 다 설명하시오.
3. 3-way handshake는 SYN, SYN/ACK, ACK로 이루어진다. 이 때 정확히 어떤 정보를 전달하는가? 정보의 형태와 무슨 내용이 들어있는가?
4. SOCK_STREAM은 소켓이 인터넷 연결의 끝점이 될 것이라는 뜻인데, 다른 타입은 어떤 게 있고, 역할은 무엇인가?
5. client의 소켓의 포트가 임시포트라면, 이 임시포트는 방화벽에 대해서 열려있어야 서버로부터 recv할 수 있는 것 아닌가? 어떻게 이루어지는가?
6. server측 listen socket과 accept socket은 포트 번호가 서로 다를 것 같다. 식별을 위해 accept socket도 서로 다를텐데, client는 이 port를 전부 알아야하는가?

## 연결 키워드

- TCP 3-way handshake
- TCP connection termination
- Ephemeral port
- Listen socket
- Connected socket
