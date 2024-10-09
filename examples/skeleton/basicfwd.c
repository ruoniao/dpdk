/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

// 默认的端口配置，设置最大接收包长度
static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
	},
};

/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default; // 使用默认端口配置
	const uint16_t rx_rings = 1, tx_rings = 1; // 设置RX和TX队列数
	uint16_t nb_rxd = RX_RING_SIZE; // 设置RX描述符数量
	uint16_t nb_txd = TX_RING_SIZE; // 设置TX描述符数量
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info; // 以太网设备信息结构体
	struct rte_eth_txconf txconf; // TX配置结构体

	// 检查端口是否有效
	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	// 获取设备信息
	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	// 如果设备支持快速释放MBUF，则启用此功能
	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	// 配置以太网设备
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	// 调整RX和TX描述符的数量
	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	// 为每个以太网端口分配并设置一个RX队列
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	// 获取默认TX配置并设置TX队列
	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	// 启动以太网端口
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	// 显示端口的MAC地址
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	// 启用以太网设备的混杂模式
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0; // 初始化成功
}

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static __rte_noreturn void
lcore_main(void)
{
	uint16_t port;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
		if (rte_eth_dev_socket_id(port) >= 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		/*
		 * Receive packets on a port and forward them on the paired
		 * port. The mapping is 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2, etc.
		 */
		RTE_ETH_FOREACH_DEV(port) {

			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
					bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0))
				continue;
			unsigned i = 0;
			for (i = 0;i < nb_rx;i ++) {
				struct rte_ether_hdr *ehdr = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr*);
				if (ehdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
					continue;
				}

				struct rte_ipv4_hdr *iphdr =  rte_pktmbuf_mtod_offset(bufs[i], struct rte_ipv4_hdr *, 
					sizeof(struct rte_ether_hdr));
				
				if (iphdr->next_proto_id == IPPROTO_UDP) {

					struct rte_udp_hdr *udphdr = (struct rte_udp_hdr *)(iphdr + 1);

					uint16_t length = ntohs(udphdr->dgram_len);
					*((char*)udphdr + length) = '\0';

					struct in_addr addr;
					addr.s_addr = iphdr->src_addr;
					printf("src: %s:%d, ", inet_ntoa(addr), udphdr->src_port);

					addr.s_addr = iphdr->dst_addr;
					printf("dst: %s:%d, %s\n", inet_ntoa(addr), udphdr->src_port, 
						(char *)(udphdr+1));

					rte_pktmbuf_free(bufs[i]);
				}
			}
			/* Send burst of TX packets, to second port of pair. */
			const uint16_t nb_tx = rte_eth_tx_burst(port ^ 1, 0,
					bufs, nb_rx);

			/* Free any unsent packets. */
			if (unlikely(nb_tx < nb_rx)) {
				uint16_t buf;
				for (buf = nb_tx; buf < nb_rx; buf++)
					rte_pktmbuf_free(bufs[buf]);
			}
		}
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool; // 内存池指针
	unsigned nb_ports; // 可用端口数
	uint16_t portid; // 端口ID

	// 初始化环境抽象层（EAL）
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	// 检查端口数量是否为偶数
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports  %d must be even\n",nb_ports);

	// 创建一个新的内存池以容纳mbuf
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	// 初始化所有端口
	RTE_ETH_FOREACH_DEV(portid)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
					portid);

	// 警告：启用了过多的逻辑核心，只使用一个
	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	// 只在主核心上调用lcore_main
	lcore_main();

	// 清理EAL
	rte_eal_cleanup();

	return 0;
}
