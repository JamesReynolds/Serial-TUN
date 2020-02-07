FROM debian:stable-slim

RUN apt-get update
RUN apt-get install -y build-essential cmake libserialport-dev libserialport0 pkg-config vim

