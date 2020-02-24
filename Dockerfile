FROM centos:7


RUN yum install -y epel-release
RUN yum update -y
RUN yum install -y which wget unzip curl tree net-tools vim git
RUN yum install -y gcc automake autoconf libtool make
RUN yum install -y openssl openssl-devel

WORKDIR /root
RUN wget https://nginx.org/download/nginx-1.16.1.tar.gz
RUN tar zxf nginx-1.16.1.tar.gz

RUN echo 'while true; do sleep 1; done' > daemon.sh
