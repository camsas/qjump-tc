/* 
* Copyright (c) 2015, Matthew P. Grosvenor
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following 
* conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following 
* disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following 
* disclaimer in the documentation and/or other materials provided with the distribution.
*
* 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products 
* derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
* INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF 
* USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED 
* OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/pkt_sched.h>
#include <linux/time.h>
#include <linux/netdevice.h>
#include <linux/moduleparam.h>

#define SEC2NS (1000 * 1000 * 1000)
#define SEC2US (1000 * 1000)
#define USEC2NS (1000)

static unsigned long bytesq = 128;
module_param(bytesq, ulong, 0);

static unsigned long timeq  = 100;
module_param(timeq, ulong, 0);

#define P0RATE_DEFAULT 1
#define P1RATE_DEFAULT 5
#define P2RATE_DEFAULT 10
#define P3RATE_DEFAULT 100
#define P4RATE_DEFAULT 1000
#define P5RATE_DEFAULT 0
#define P6RATE_DEFAULT 0
#define P7RATE_DEFAULT 10000

static unsigned long p0rate = P0RATE_DEFAULT;
static unsigned long p1rate = P1RATE_DEFAULT;
static unsigned long p2rate = P2RATE_DEFAULT;
static unsigned long p3rate = P3RATE_DEFAULT;
static unsigned long p4rate = P4RATE_DEFAULT;
static unsigned long p5rate = P5RATE_DEFAULT;
static unsigned long p6rate = P6RATE_DEFAULT;
static unsigned long p7rate = P7RATE_DEFAULT;

module_param(p0rate, ulong, 0);
module_param(p1rate, ulong, 0);
module_param(p2rate, ulong, 0);
module_param(p3rate, ulong, 0);
module_param(p4rate, ulong, 0);
//module_param(p5rate, ulong, 0);
//module_param(p6rate, ulong, 0);
module_param(p7rate, ulong, 0);


static unsigned long autoclass = 0;
module_param(autoclass, ulong, 0);


static unsigned long verbose = 0;
module_param(verbose, ulong, 0);

static u64 frequency = 0;
static u64 time_quant_cyles = 0;



//Map of prioirty level to the packet rate multipler.
//Assumes >= 8 queues. This should be generalized across nics.
uint64_t prates_map[8] = { 0 };

struct qjump_fifo_priv{
    u64 bytes_left;
    u64 next_timeout_cyles;
    int drop; 
    #define CYCLE_STATS_LEN (128)
    u64 cycles_consumed[CYCLE_STATS_LEN];
    u64 index;
};


static int qfifo_enqueue(struct sk_buff *skb, struct Qdisc *sch)
{
    struct qjump_fifo_priv* priv = qdisc_priv(sch);
    struct timespec ts;
    const u64 ts_now_cycles = get_cycles();
    u64 ts_now_ns = 0;
    getnstimeofday(&ts);
    ts_now_ns = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;

    if(ts_now_cycles >= priv->next_timeout_cyles){
        priv->next_timeout_cyles = ts_now_cycles + time_quant_cyles;

        if(verbose>=3) printk("QJump[%lu]: (%u) Reset: %u,%u,%u,%llu,%llu,%llu,%lli,%llu\n",
                              verbose, sch->limit, sch->qstats.backlog, qdisc_pkt_len(skb), sch->limit, priv->bytes_left,
                              priv->next_timeout_cyles , ts_now_cycles, (priv->next_timeout_cyles - ts_now_cycles), ts_now_ns
                              );
        priv->bytes_left      = sch->limit;
        priv->drop            = 0;
    }

    if (likely(qdisc_pkt_len(skb) <= priv->bytes_left)){
        int ret = 0;
        if(verbose>=3) printk("QJump[%lu]: (%u) Forwarded: %u,%u,%u,%llu,%llu,%llu,%lli,%llu\n",
                              verbose, sch->limit, sch->qstats.backlog, qdisc_pkt_len(skb), sch->limit, priv->bytes_left,
                              priv->next_timeout_cyles , ts_now_cycles, (priv->next_timeout_cyles - ts_now_cycles), ts_now_ns
                              );
        ret = qdisc_enqueue_tail(skb, sch);
        priv->bytes_left -= qdisc_pkt_len(skb);
        priv->cycles_consumed[priv->index]= get_cycles() - ts_now_cycles;
        priv->index = (priv->index + 1) % (CYCLE_STATS_LEN);
        return ret;
    }

    if((1 || (!priv->drop && verbose >=1)) || verbose >= 3){
        if(verbose>=2)	printk("QJump[%lu]: (%u) Dropped: %u,%u,%u,%llu,%llu,%llu,%lli,%llu\n",
                               verbose, sch->limit, sch->qstats.backlog, qdisc_pkt_len(skb), sch->limit, priv->bytes_left,
                               priv->next_timeout_cyles , ts_now_cycles, (priv->next_timeout_cyles - ts_now_cycles),
                               ts_now_ns
                               );
        priv->drop = 1;
    }

    sch->qstats.drops++;
    priv->cycles_consumed[priv->index]= get_cycles() - ts_now_cycles;
    priv->index = (priv->index + 1) % (CYCLE_STATS_LEN);
    return NET_XMIT_DROP;

}


static int qfifo_init(struct Qdisc *sch, struct nlattr *opt)
{
    sch->flags &= ~TCQ_F_CAN_BYPASS;
    return 0;
}


struct Qdisc_ops qjump_fifo_qdisc_ops __read_mostly = {
        .id         =   "qjump_fifo",
        .priv_size  =   sizeof(struct qjump_fifo_priv),
        .enqueue    =   qfifo_enqueue,
        .dequeue    =   qdisc_dequeue_head,
        .peek       =   qdisc_peek_head,
        .drop       =   qdisc_queue_drop,
        .init       =   fifo_init,
        .reset      =   qdisc_reset_queue,
        .change     =   qfifo_init,
};


struct Qdisc *qjump_fifo_create_dflt(struct netdev_queue *dev_queue, struct Qdisc *sch, struct Qdisc_ops *ops, unsigned int limit)
{
    struct Qdisc *q;
    int err = -ENOMEM;
    //struct timespec ts; //Allow QJump to be run directly from getnstimeofday()


    if(verbose >= 1) printk("qjump[%lu]: Init fifo limit=%u\n", verbose, limit);

    q = qdisc_create_dflt(dev_queue, ops, TC_H_MAKE(sch->handle, 1));
    if (q) {
        struct qjump_fifo_priv* priv = qdisc_priv(q);
        u64 ts_now_cycles = 0;
        q->limit = limit;
        priv->bytes_left = limit;
        priv->drop = 0; 
        priv->index = 0;
        //ts_now_cycles = get_cycles();
        //getnstimeofday(&ts);
        //ts_now_cycles = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;  //get_cycles();
        ts_now_cycles = get_cycles();

        priv->next_timeout_cyles = ts_now_cycles + time_quant_cyles;

    }

    return q ? : ERR_PTR(err);
}


struct qjump_sched_data {
    u16 bands;
    u16 max_bands;
    u16 curband;
    struct tcf_proto *filter_list;
    struct Qdisc **queues;
};


static struct Qdisc * qjump_classify(struct sk_buff *skb, struct Qdisc *sch, int *qerr)
{
    struct qjump_sched_data *q = qdisc_priv(sch);

    skb->priority = skb->priority >= q->bands ? q->bands - 1 : skb->priority;
    skb->queue_mapping = skb->priority;
    if(verbose >= 5) printk("QJump[%lu]: Classify priority=%u queue_mapping=%i\n", verbose, skb->priority, skb->queue_mapping);

    return q->queues[skb->priority];
}


static int qjump_enqueue(struct sk_buff *skb, struct Qdisc *sch)
{
    struct Qdisc *qdisc;
    int ret;

    qdisc = qjump_classify(skb, sch, &ret);

    ret = qdisc->enqueue(skb, qdisc);
    if (ret == NET_XMIT_SUCCESS) {
        sch->q.qlen++;
        return NET_XMIT_SUCCESS;
    }


    //Auto classifer will keep trying to send you until a) you reach 0 or b) you send.
    while(autoclass && skb->priority){
        int old_pri = skb->priority;

        //Deal with the discontinuity between 3 and 0
        if(skb->priority == 3){
            skb->priority = 1;
        }

        skb->priority--;


        if(verbose>=2){
            u64 ts_now_ns = 0;
            struct timespec ts = {0};
            getnstimeofday(&ts);
            ts_now_ns = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
            printk("QJump[%lu]: (%u) Downgraded: %u,%u,%u,%llu",
                   verbose, qdisc->limit,  qdisc_pkt_len(skb), old_pri, skb->priority, ts_now_ns
                   );
        }


        qdisc = qjump_classify(skb, sch, &ret);
        ret = qdisc->enqueue(skb, qdisc);
        if (ret == NET_XMIT_SUCCESS) {
            skb->sk->sk_priority = skb->priority; //Make sure we come here next time.
            sch->q.qlen++;
            return NET_XMIT_SUCCESS;
        }
    }

    return qdisc_reshape_fail(skb, sch);
}

static struct sk_buff *qjump_dequeue(struct Qdisc *sch)
{

    struct qjump_sched_data *q = qdisc_priv(sch);
    struct Qdisc *qdisc;
    struct sk_buff *skb;
    int band;

    for (band = 0; band < q->bands; band++) {

        if (!__netif_subqueue_stopped(qdisc_dev(sch), q->bands - 1 - band)) {
            qdisc = q->queues[q->bands - 1 - band];
            skb = qdisc->dequeue(qdisc);
            if (skb) {
                qdisc_bstats_update(sch, skb);
                sch->q.qlen--;
                return skb;
            }
        }
    }
    return NULL;

}

static struct sk_buff *qjump_peek(struct Qdisc *sch)
{
    struct qjump_sched_data *q = qdisc_priv(sch);
    struct Qdisc *qdisc;
    struct sk_buff *skb;
    int band;

    for (band = 0; band < q->bands; band++) {

        /* Check that target subqueue is available before
         * pulling an skb to avoid head-of-line blocking.
         */
        if (!__netif_subqueue_stopped(qdisc_dev(sch), q->bands - 1 - band)) {
            qdisc = q->queues[q->bands - 1 - band];
            skb = qdisc->ops->peek(qdisc);
            if (skb)
                return skb;
        }
    }
    return NULL;

}

static unsigned int qjump_drop(struct Qdisc *sch)
{
    struct qjump_sched_data *q = qdisc_priv(sch);
    int band;
    unsigned int len;
    struct Qdisc *qdisc;

    for (band = q->bands - 1; band >= 0; band--) {
        qdisc = q->queues[band];
        if (qdisc->ops->drop) {
            len = qdisc->ops->drop(qdisc);
            if (len != 0) {
                sch->q.qlen--;
                return len;
            }
        }
    }
    return 0;
}


static void qjump_reset(struct Qdisc *sch)
{
    u16 band;
    struct qjump_sched_data *q = qdisc_priv(sch);

    for (band = 0; band < q->bands; band++)
        qdisc_reset(q->queues[band]);
    sch->q.qlen = 0;
    q->curband = 0;
}

static void qjump_destroy(struct Qdisc *sch)
{
    int band = 0;
    struct qjump_sched_data *q = qdisc_priv(sch);

    if(verbose >= 1) printk("qjump[%lu]: Destroying queues\n", verbose);

    printk("QJump[STATS] --------------------------------------------\n");
    printk("QJump[STATS] frequency %lluHz", frequency);
    if (q->queues) {
        for (band = 0; band < q->bands; band++){
            struct Qdisc* sub = q->queues[band];
            struct qjump_fifo_priv* priv = qdisc_priv(sub);
            int i = 0;
            for(i=0; i < CYCLE_STATS_LEN; i++ ){
                printk("QJump[STATS] %i %i %llu\n", band, i, priv->cycles_consumed[i]);
            }

            if(verbose >= 2) printk("QJump[%lu]: Destroying queues %i\n", verbose, band);
            sub->ops->reset(sub);
            dev_put(qdisc_dev(sub));
            kfree_skb(sub->gso_skb);

        }

        kfree(q->queues);
    }
    printk("QJump[STATS] --------------------------------------------\n");

    //qdisc_dev(sch)->netdev_ops->ndo_setup_tc(qdisc_dev(sch), 0);
    netdev_set_num_tc(qdisc_dev(sch), 0);
}



static int qjump_init(struct Qdisc *sch, struct nlattr *opt)
{
    struct qjump_sched_data *q = qdisc_priv(sch);
    int i; //, err;
    struct net_device *dev = qdisc_dev(sch);
    struct netdev_queue *dev_queue;

    if(verbose >= 1 ) printk("QJump[%lu]: Init QJump QDisc", verbose);

    q->queues = NULL;
    q->max_bands = qdisc_dev(sch)->real_num_tx_queues;
    q->max_bands = q->max_bands > 8 ? 8 : q->max_bands;

    //Assign the packet rate map
    prates_map[0] = p0rate;
    prates_map[1] = p1rate;
    prates_map[2] = p2rate;
    prates_map[3] = p3rate;
    prates_map[4] = p4rate;
    prates_map[5] = p5rate;
    prates_map[6] = p6rate;
    prates_map[7] = p7rate;

    time_quant_cyles = timeq * frequency / SEC2US;
    if(verbose >=0 ) printk("QJump[%lu]: Delaying %llu cycles per network tick (%lluus)\n", verbose, time_quant_cyles, (time_quant_cyles * 1000 * 1000) / frequency );

    q->queues = kcalloc(q->max_bands, sizeof(struct Qdisc *), GFP_KERNEL);
    if (!q->queues){
        return -ENOBUFS;
    }


    for (i = 0; i < q->max_bands; i++){
        if(verbose >= 0) printk("QJump[%lu]: Queue %u = @ %lluMb/s \n", verbose, q->max_bands -1 - i, prates_map[i]*bytesq * 8 / timeq );
        dev_queue = netdev_get_tx_queue(dev, q->max_bands -1 - i);
        q->queues[q->max_bands -1 - i] = qjump_fifo_create_dflt(dev_queue,sch,&qjump_fifo_qdisc_ops,prates_map[i]*bytesq);
    }


    q->bands = q->max_bands;
    printk("Bands= %u\n", q->bands);

    return 0; //err;
}

struct Qdisc_ops qjump_qdisc_ops __read_mostly = {
        .next		=	NULL,
        .id		    =	"qjump",
        .priv_size	=	sizeof(struct qjump_sched_data),
        .enqueue	=	qjump_enqueue,
        .dequeue	=	qjump_dequeue,
        .peek		=	qjump_peek,
        .drop		=	qjump_drop,
        .init		=	qjump_init,
        .reset		=	qjump_reset,
        .destroy	=	qjump_destroy,
        .owner		=	THIS_MODULE,
};

static int __init qjump_module_init(void)
{
    struct timespec ts;
    u64 ts_start_ns = 0;
    u64 ts_end_ns = 0;
    u64 start_cycles = 0;
    u64 end_cycles = 0;
    int has_invariant_tsc = 0;
    unsigned int eax, ebx, ecx, edx;
    int i = 0;

    if(verbose >= 1) printk("QJump[%lu]: Init module\n", verbose);

    asm("cpuid" : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (0x80000007));
    has_invariant_tsc = edx & (1 << 8);

    if(!has_invariant_tsc){
        printk("QJump[%lu]: Cannot run qjump on machines without an invaraint TSC. Terminating\n", verbose);
        return -1;
    }

    for(i = 0; i <3; i++){
        getnstimeofday(&ts);
        ts_start_ns = (ts.tv_nsec + ts.tv_sec * SEC2NS );
        if(verbose >= 0 ) printk("QJump[%lu]: Calculating CPU speed %i\n", verbose, i);
        start_cycles = get_cycles();
        while(1){
            getnstimeofday(&ts);
            ts_end_ns = (ts.tv_nsec + ts.tv_sec * SEC2NS );
            if((ts_end_ns - ts_start_ns) >= 1 * SEC2NS){
                end_cycles = get_cycles();
                break;
            }
        }
        frequency = end_cycles - start_cycles;
        if(verbose >= 0 ) printk("QJump[%lu]: CPU is running at %llu cyles per second\n", verbose, frequency);
    }

    if(verbose >= 1) printk("QJump: Module parameteres:\n");
    if(verbose >= 1) printk("-------------------------------\n");
    if(verbose >= 1) printk("QJump: timeq=%luus\n", timeq);
    if(verbose >= 1) printk("QJump: bytesq=%luB\n", timeq);
    if(verbose >= 1) printk("QJump: p7rate=%lu\n", p7rate);
    if(verbose >= 1) printk("QJump: p6rate=%lu\n", p6rate);
    if(verbose >= 1) printk("QJump: p5rate=%lu\n", p5rate);
    if(verbose >= 1) printk("QJump: p4rate=%lu\n", p4rate);
    if(verbose >= 1) printk("QJump: p3rate=%lu\n", p3rate);
    if(verbose >= 1) printk("QJump: p2rate=%lu\n", p2rate);
    if(verbose >= 1) printk("QJump: p1rate=%lu\n", p1rate);
    if(verbose >= 1) printk("QJump: p0rate=%lu\n", p0rate);
    if(verbose >= 1) printk("-------------------------------\n\n");

    return register_qdisc(&qjump_qdisc_ops);
}

static void __exit qjump_module_exit(void)
{
    printk("QJump[%lu]: QJump Module exit\n", verbose);
    unregister_qdisc(&qjump_qdisc_ops);
}

module_init(qjump_module_init)
module_exit(qjump_module_exit)

MODULE_LICENSE("BSD");
