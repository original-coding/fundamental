# rudp 连接控制指令
## RUDP_SYN
客户端连接服务器时发送
## RUDP_SYN_ACK
## RUDP_SYN_ACK2
## RUDP_FIN
## RUDP_FIN_ACK
## RUDP_PING
## RUDP_PONG

# rudp 连接状态
## RUDP_CLOSED
## RUDP_SYN_SENT
## RUDP_SYN_RECV
## RUDP_CONNECTED
## RUDP_FIN_SENT
## RUDP_PING_SENT
# rudp 控制参数
## RUDP_CONNECT_TIMEOUT_MS
有返回确认的指令超时时间默认3000ms 最小值10ms
## RUDP_COMMAND_MAX_TRY_CNT
有返回确认的指令最大超时次数，超过此次数后会进入关闭流程，默认为2
## RUDP_MAX_SEND_WINDOW_SIZE
最大缓存窗口大小，此处对应的是待发送的包的个数，如果缓存待发送数据超过此数量的2倍，进入关闭流程，默认为256
## RUDP_MAX_RECV_WINDOW_SIZE
最大接收窗口大小，此处对应的是能够缓存对方包的个数
## RUDP_MTU_SIZE
最大的rudp包的大小，此数据由客户端与服务器协商，取两者的小值
## RUDP_ENABLE_NO_DELAY
## RUDP_UPDATE_INTERVAL_MS
## RUDP_FASK_RESEND_SKIP_CNT
## RUDP_ENABLE_NO_CONGESTION_CONTROL