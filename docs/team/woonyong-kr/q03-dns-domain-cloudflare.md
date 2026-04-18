# Q05. DNS, 도메인 등록, Cloudflare 로 접속이 되기까지의 원리

> CSAPP 11.3 | Domain Name System + 실무 연결 | 중급

## 질문

1. DNS 는 무엇이고 어떤 용도로 쓰이는가.
2. 도메인 이름은 어떻게 **등록(소유)** 되고, 어떻게 IP 로 **해석(resolve)** 되는가.
3. Cloudflare 에서 도메인을 구매하고 DNS 를 등록하면 접속이 가능해지는데, 그 과정을 단계별로 설명해 달라.

## 답변

### 최우녕

> DNS 는 무엇이고 어떤 용도로 쓰이는가.

DNS(Domain Name System)는 "**사람이 읽는 도메인 이름 <-> 기계가 쓰는 IP 주소**"를 매핑해주는 **전 세계 분산 데이터베이스**다. 인터넷에서 주소록 역할을 한다.

용도는 크게 넷이다.

- **A / AAAA 레코드**: 도메인 -> IPv4 / IPv6 매핑. 가장 기본적인 용도.
- **CNAME**: 도메인 -> 다른 도메인 별칭. (예: `www.example.com -> example.com`)
- **MX**: 메일 서버 호스트. 이메일 라우팅에 사용.
- **TXT**: 임의 텍스트. 도메인 소유권 증명(SPF, DKIM, ACME challenge 등).

DNS 가 없으면 사람이 IP 를 외워야 하고, 서버를 옮길 때마다 모든 클라이언트의 설정을 바꿔야 한다. DNS 가 있으므로 "도메인 이름" 이라는 **간접 참조 계층**을 하나 더 두고, IP 가 바뀌어도 DNS 만 고치면 된다.

> 도메인 이름은 어떻게 등록되고, 어떻게 IP 로 해석되는가.

**(1) 등록 쪽** 은 계약/권한의 세계다.

```text
ICANN (전체 규칙 관리)
   │
   v
Registry (TLD 관리: .com 은 Verisign, .kr 은 KISA 등)
   │
   v
Registrar (도메인 판매자: Cloudflare, GoDaddy, Gabia ...)
   │
   v
Registrant (나 = 도메인 구매자)
```

내가 `example.net` 을 사면, Registrar 가 Registry 에 "이 도메인의 네임서버는 X" 라고 등록해준다. 이 "네임서버 지정(NS record)" 이 **도메인 권한 위임**의 핵심이다. 누군가 `example.net` 을 물으면 `.net` TLD 네임서버가 "그건 X 네임서버에 물어봐" 라고 안내해준다.

**(2) 해석(resolve) 쪽** 은 질의/응답의 세계다. 클라이언트가 `www.example.net` 을 풀려고 하면 아래 순서로 재귀 질의가 일어난다.

```text
브라우저
   │  1. gethostbyname / getaddrinfo("www.example.net")
   v
stub resolver (libc)
   │  2. /etc/hosts, /etc/nsswitch.conf 확인
   │  3. systemd-resolved, 또는 /etc/resolv.conf 의 DNS 서버에 UDP 53 질의
   v
Recursive resolver  (ISP, 또는 1.1.1.1, 8.8.8.8)
   │
   │  4. 캐시 miss 이면 루트로 올라간다
   v
Root nameserver (.) ── "com 은 .com TLD 에게 물어봐"
   │
   v
TLD nameserver (.net) ── "example.net 의 네임서버는 ns1.cloudflare.com"
   │
   v
Authoritative nameserver (Cloudflare) ── "www.example.net 은 208.216.181.15"
   │
   v
Recursive resolver  ──-> stub resolver ──-> 브라우저
   │                                       │
   v                                       v
결과 IP 획득 (+ TTL 동안 캐싱)         connect(208.216.181.15:80)
```

실제 명령으로 확인할 수 있다.

```bash
$ dig www.example.net +trace
$ nslookup www.example.net
```

포인트 세 가지:

- DNS 는 **UDP 포트 53** 을 기본으로 쓰고, 대용량 응답엔 TCP 53 을 쓴다. 최근엔 DoT(853, TCP/TLS), DoH(443, HTTPS) 도 흔하다.
- 각 응답에는 **TTL** 이 있어서 일정 시간 동안 resolver 가 캐싱한다. 그래서 DNS 변경이 전 세계에 반영되는 데 시간이 걸린다.
- 같은 도메인을 여러 IP 로 응답할 수 있다(round-robin, GeoDNS).

> Cloudflare 에서 도메인을 구매하고 DNS 를 등록하면 접속이 가능해지는데, 그 과정을 단계별로 설명해 달라.

전체 흐름을 "등록 단계" 와 "조회 단계" 로 나눠 보자.

**등록 단계 (사람이 하는 일)**

```text
1) Cloudflare Registrar 에서 example.net 구매
     ㄴ ICANN 규정대로 WHOIS 정보 입력
     ㄴ Cloudflare 가 .net Registry(Verisign) 에 "example.net 을 우녕이 샀다" 기록 요청

2) Cloudflare 가 .net TLD 에 NS 레코드 등록
     example.net.   IN NS   nina.ns.cloudflare.com
     example.net.   IN NS   tom.ns.cloudflare.com

3) 내가 Cloudflare DNS 대시보드에서 레코드 추가
     www.example.net   A     208.216.181.15   (TTL 300)
     example.net       A     208.216.181.15
     example.net       MX    10 mail.example.net

4) 위 레코드들은 Cloudflare 의 authoritative nameserver 에 저장됨
```

**조회 단계 (사용자가 브라우저로 접속할 때)**

```text
사용자가 브라우저에 http://www.example.net 입력

1) 브라우저/OS 캐시 확인 -> miss
2) stub resolver 가 /etc/resolv.conf 의 nameserver 에 질의
     -> ISP resolver 또는 1.1.1.1 (recursive)
3) recursive resolver 가 루트 -> .net -> Cloudflare NS 까지 재귀 질의
4) Cloudflare NS 가 www.example.net -> 208.216.181.15 응답 (+ TTL)
5) 브라우저가 208.216.181.15:80 으로 TCP connect + HTTP GET
6) 서버가 HTML 응답 -> 렌더링
```

여기서 Cloudflare 가 특별한 이유는 두 가지다.

- **Registrar + Authoritative NS + CDN 프록시**가 한 몸이라 "도메인 사면 자동으로 CF 네임서버에 붙는" 일관된 경험을 준다.
- "Proxied" (주황색 구름) 상태로 둔 레코드는 실제 IP 가 아니라 **Cloudflare 자체 엣지 IP**가 응답된다. 즉 외부에서 보면 `www.example.net -> 104.x.x.x (Cloudflare 엣지)` 로 보이고, Cloudflare 가 뒤에서 오리진 서버(`208.216.181.15`) 로 요청을 대신 보낸다. 이게 DDoS 완화, 캐싱, TLS 종단점 역할까지 하는 원리다.

결론적으로 **"도메인을 샀다"** = registrar 가 TLD 에 NS 권한을 등록해준 것이고, **"DNS 에 등록했다"** = 그 NS 에 A/CNAME/MX 레코드를 써둔 것이다. 브라우저 접속은 이 두 가지를 따라 재귀 질의로 IP 를 알아낸 뒤 TCP+HTTP 로 이어지는 것.

## 연결 키워드

- [02-keyword-tree.md — DNS 섹션](../../csapp-11/02-keyword-tree.md)
- q04. IP 와 바이트 순서 (resolve 결과를 어떻게 소켓에 넣는지)
- q07. getaddrinfo 가 내부적으로 DNS 를 호출한다
