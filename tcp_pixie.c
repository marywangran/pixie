#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet.h>

static int rate = 100000000;
module_param(rate, int, 0644);
static int feedback = 2;
module_param(feedback, int, 0644);

struct sample {
	u32	_acked;
	u32	_losses;
	u32	_tstamp_us;
};

struct pixie {
	u64	rate;
	u16	start;
	u16	end;
	u32	curr_acked;
	u32	curr_losses;
	struct sample *samples;
};

static void pixie_main(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct pixie *pixie = inet_csk_ca(sk);
	u32 now = tp->tcp_mstamp;
	u32 cwnd;
	u16 start, end;
	u64 prate;

	if (rs->delivered < 0 || rs->interval_us <= 0)
		return;

	cwnd = pixie->rate;
	if (!pixie->samples) {
		cwnd /= tp->mss_cache;
		cwnd *= (tp->srtt_us >> 3);
		cwnd /= USEC_PER_SEC;
		tp->snd_cwnd = min(2 * cwnd, tp->snd_cwnd_clamp);
		sk->sk_pacing_rate = min_t(u64, pixie->rate, READ_ONCE(sk->sk_max_pacing_rate));
		return;
	}

  // 丢包突然增加时采用紧缩策略，以在保吞吐时尽量减少进一步丢包，丢包突然减少时采用激进策略，以挤占比实际更多资源。移动平均是个好办法。
	pixie->curr_acked += rs->acked_sacked;
	pixie->curr_losses += rs->losses;
	end = pixie->end ++;
	pixie->samples[end]._acked = rs->acked_sacked;
	pixie->samples[end]._losses = rs->losses;
	pixie->samples[end]._tstamp_us = now;

	start = pixie->start;
	while (start < end) {
    // 至少保持半个 srtt 反馈周期，越久越不抖动但性能可能不达预期，这里的 “抖动” 要反着理解
		if (2 * (now -  pixie->samples[start]._tstamp_us) > feedback * tp->srtt_us) {
			pixie->curr_acked -= pixie->samples[start]._acked;
			pixie->curr_losses -= pixie->samples[start]._losses;
			pixie->start ++;
		}
		start ++;
	}
	cwnd /= tp->mss_cache;
	cwnd *= pixie->curr_acked + pixie->curr_losses;
	cwnd /= pixie->curr_acked;
	cwnd *= (tp->srtt_us >> 3);
	cwnd /= USEC_PER_SEC;

	prate = (pixie->curr_acked + pixie->curr_losses) << 10;
	prate /= pixie->curr_acked;
	prate *= pixie->rate;
	prate = prate >> 10;

	printk("##### curr_ack:%llu curr_loss:%llu rsloss:%llu satrt:%llu  end:%llu cwnd:%llu rate:%llu prate:%llu\n",
			pixie->curr_acked,
			pixie->curr_losses,
			rs->losses,
			pixie->start,
			pixie->end,
			cwnd,
			rate,
			prate);
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
  // 用 pacing_rate 去挤兑(而不是 cwnd)，促使单位时间内到达报文更多，以抵消丢包损耗。如果用 cwnd 的话就别 pacing。
	sk->sk_pacing_rate = min_t(u64, prate, sk->sk_max_pacing_rate);
  //sk->sk_pacing_rate = min_t(u64, pixie->rate, sk->sk_max_pacing_rate);
}

static void pixie_init(struct sock *sk)
{
	struct pixie *pixie = inet_csk_ca(sk);

	pixie->rate = (u64)rate;
	pixie->start = 0;
	pixie->end = 0;
	pixie->curr_acked = 0;
	pixie->curr_losses = 0;
	pixie->samples = kmalloc(U16_MAX * sizeof(struct sample), GFP_ATOMIC);
	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void pixie_release(struct sock *sk)
{
	struct pixie *pixie = inet_csk_ca(sk);

	if (pixie->samples)
		kfree(pixie->samples);
}


static u32 pixie_ssthresh(struct sock *sk)
{
	return TCP_INFINITE_SSTHRESH;
}

static u32 pixie_undo_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	return tp->snd_cwnd;
}

static struct tcp_congestion_ops tcp_pixie_cong_ops __read_mostly = {
	.flags		= TCP_CONG_NON_RESTRICTED,
	.name		= "pixie",
	.owner		= THIS_MODULE,
	.init		= pixie_init,
	.release	= pixie_release,
	.cong_control	= pixie_main,
	.ssthresh	= pixie_ssthresh,
	.undo_cwnd 	= pixie_undo_cwnd,
};

static int __init pixie_register(void)
{
	BUILD_BUG_ON(sizeof(struct pixie) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_pixie_cong_ops);
}

static void __exit pixie_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_pixie_cong_ops);
}

module_init(pixie_register);
module_exit(pixie_unregister);
MODULE_LICENSE("GPL");
