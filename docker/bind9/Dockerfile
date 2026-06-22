FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y bind9 bind9utils dnsutils && \
    rm -rf /var/lib/apt/lists/*

COPY named.conf         /etc/bind/named.conf
COPY named.conf.options /etc/bind/named.conf.options
COPY named.conf.local   /etc/bind/named.conf.local
COPY zones/db.example.local /var/lib/bind/db.example.local

RUN chown -R bind:bind /var/lib/bind /etc/bind && \
    named-checkconf /etc/bind/named.conf

EXPOSE 53/udp 53/tcp

HEALTHCHECK --interval=2s --timeout=3s --retries=15 \
    CMD dig @127.0.0.1 example.local SOA +short || exit 1

CMD ["/usr/sbin/named", "-f", "-u", "bind", "-c", "/etc/bind/named.conf"]
