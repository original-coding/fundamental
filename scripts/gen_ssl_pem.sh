#!/bin/bash
####################CA##########################

#生成 ca 私钥
openssl genrsa -out ca_root.key 2048

#生成CA机构自己的证书申请文件
openssl req -new -out ca_root.csr -key ca_root.key -config openssl_ca.cnf

#生成自签名证书==>CA机构用自己的私钥和证书申请文件生成自己签名的证书,俗称自签名证书,这里可以理解为根证书ca_root.crt
openssl x509 -req  -in ca_root.csr -signkey ca_root.key  -out ca_root.crt  -extfile openssl_ca.cnf -days 3650 -sha256

####################Server##########################
#生成服务器私钥
openssl genrsa -out server.key 2048


# 生成服务器证书请求
openssl req -new -key server.key -out server.csr -config openssl_server.cnf

# 使用ca签署服务器证书
openssl x509 -req -in server.csr -CA ca_root.crt -CAkey ca_root.key -CAcreateserial -out server.crt -extfile openssl_server.cnf -extensions v3_req -days 3650 -sha256 



####################Client##########################
#生成客户端私钥
openssl genrsa -out client.key 2048


# 生成客户端证书请求
openssl req -new -key client.key -out client.csr -config openssl_client.cnf

# 使用ca签署客户端证书
openssl x509 -req -in client.csr -CA ca_root.crt -CAkey ca_root.key -CAcreateserial -out client.crt -extfile openssl_client.cnf -extensions v3_req  -days 3650 -sha256


#生成dh密钥文件
openssl dhparam -out dh2048.pem 2048

#查看证书内容
# openssl x509 -in server.crt -text -noout

#查看dh参数
# openssl dhparam -in dh2048.pem -text -noout

#验证证书是否由ca签发
# openssl verify -CAfile ca_root.crt client.crt

#验证服务器的ssl特性
# openssl s_client -connect 127.0.0.1:9000 -CAfile ca_root.crt -cert client.crt -key client.key