#ifndef ns_prio_dwrr_h
#define ns_prio_dwrr_h

#include "queue.h"
#include "config.h"
#include "trace.h"
#include "timer-handler.h"

/* Maximum number of strict higher priority queues */
#define MAX_PRIO_QUEUE_NUM 8
/* Maximum number of DWRR queues in the lowest priority */
#define MAX_DWRR_QUEUE_NUM 64

// Per-queue ECN marking
#define PER_QUEUE_MARKING 0
// Per-port ECN marking
#define PER_PORT_MARKING 1
// TCN
#define TCN_MARKING 2
// MQ-ECN
#define MQ_ECN_MARKING 3

/* Types of queues */
#define PRIO_QUEUE 0
#define DWRR_QUEUE 1

class PacketPRIO;
class PacketDWRR;
class PRIO_DWRR;

class PacketPRIO: public PacketQueue
{
	public:
		PacketPRIO(): thresh(0) {}

		int id;	//priority queue ID
		double thresh;	//per-queue ECN marking threshold (pkts)

		friend class PRIO_DWRR;
};

class PacketDWRR: public PacketQueue
{
	public:
		PacketDWRR(): thresh(0), quantum(1500), deficit(0), start_time(0), next(NULL) {}

		int id;
                double thresh;	// per-queue ECN marking threshold (pkts)
		int quantum;	// quantum of this queue
		int deficit;	// deficit counter for this queue
		double start_time;	// time when this queue is inserted to active list
                PacketDWRR *next;	// pointer to next node

		friend class PRIO_DWRR;
};

class PRIO_DWRR : public Queue
{
	public:
		PRIO_DWRR();
		~PRIO_DWRR();
		virtual int command(int argc, const char*const* argv);

	protected:
		Packet *deque(void);
		void enque(Packet *pkt);
		int total_bytelength();	//total length of all the queues in bytes
		int dwrr_bytelength();	//total length of DWRR queues in bytes
		int prio_bytelength();	//total length of higher priority queues in bytes
		int ecn_mark(int queue_index);	//queue length ECN marking
		void tcn_mark(Packet *pkt);	//our solution: TCN
		void reset_roundtime();	//reset round time of MQ-ECN

		PacketPRIO *prio_queues;	//strict higher priority queues
		PacketDWRR *dwrr_queues;	//DWRR queues in the lowest priority
		PacketDWRR *active;	//list for active DWRR queues

		int dwrr_queue_num_;	//number of DWRR queues
		int prio_queue_num_;	//number of higher priority queues

                int mean_pktsize_;	//MTU in bytes
                int marking_scheme_;	//ECN marking policy
                double port_thresh_;	//per-port ECN marking threshold (pkts)
                double link_capacity_;  //Link capacity
                int debug_;     //debug more(true) or not(false)

                // MQ-ECN
		double round_time;    //estimation value for round time
		double last_idle_time;	//Last time when link becomes idle
		double mqecn_alpha_;	//alpha for MQ-ECN
		int mqecn_interval_bytes_;	//interval (divided by link capacity) for MQ-ECN

		Tcl_Channel total_qlen_tchan_;        //place to write total_qlen records
		Tcl_Channel qlen_tchan_;      //place to write per-queue qlen records
		void trace_total_qlen();      //routine to write total qlen records
		void trace_qlen();	//routine to write per-queue qlen records
};

#endif
