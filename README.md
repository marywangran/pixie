# pixie
尽力保持吞吐(有效带宽)的 TCP 发包算法，pixie 不是 “皮鞋”，而是 “捣蛋鬼”.

- 算法细节：
  - pacing_rate = rate * (losses + acked)/(acked)
  - cwnd = rate * (losses + acked)/(acked) * rtt


## 安装
- 安装内核头文件: apt install kernel-package
- make
- insmod ./tcp_pixie.ko
- echo 200000000 >/sys/module/tcp_pixie/parameters/rate # 保 200MBps 吞吐

## 测试
- iperf -s 端设置丢包：iptables -I INPUT -p tcp --dport 5001 -m statistic --mode random --probability 0.09 -j DROP
- iperf -c x.x.x.x - i 1 -Z pixie
