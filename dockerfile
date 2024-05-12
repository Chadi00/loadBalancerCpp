FROM alpine:latest

RUN apk add --no-cache \
    g++ \
    make \
    cmake \
    linux-headers \
    git \
    bash \
    util-linux \
    gcompat # Adding gcompat for compatibility with glibc

WORKDIR /app

COPY . .

RUN g++ -std=c++17 -pthread -o loadbalancer main.cpp

EXPOSE 8080

CMD ["./loadbalancer"]
