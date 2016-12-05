#ifndef ns_prio_wfq_h
#define ns_prio_wfq_h

#include "queue.h"
#include "config.h"
#include "trace.h"
#include "timer-handler.h"

#include <iostream>
#include <queue>
using namespace std;

/* Maximum number of strict higher priority queues */
#define MAX_PRIO_QUEUE_NUM 8
/* Maximum number of WFQ queues in the lowest priority */
#define MAX_WFQ_QUEUE_NUM 64

/* Per-queue ECN marking */
#define PER_QUEUE_MARKING 0
/* Per-port ECN marking */
#define PER_PORT_MARKING 1
/* TCN ECN marking */
#define TCN_MARKING 2

/* Types of queues */
#define PRIO_QUEUE 0
#define WFQ_QUEUE 1

class PacketPRIO;	//strict higher priority queues
class PacketWFQ;	//WFQ queues in the lowest priority
class PRIO_WFQ;

class PacketPRIO: public PacketQueue
{
	public:
		PacketPRIO(): thresh(0) {}

		double thresh;	//per-queue ECN marking threshold (pkts)
		friend class PRIO_WFQ;
};

class PacketWFQ : public PacketQueue
{
	public:
		PacketWFQ(): weight(10000.0), headFinishTime(0), thresh(0) {}

		double weight;    //weight of the service
  		long double headFinishTime; //finish time of the packet at head of this queue.
		double thresh;    //per-queue ECN marking threshold (pkts)

		friend class PRIO_WFQ;
};

class PRIO_WFQ : public Queue
{
	public:
		PRIO_WFQ();
		~PRIO_WFQ();
		virtual int command(int argc, const char*const* argv);

	protected:
		Packet* deque(void);
		void enque(Packet *pkt);
		int total_bytelength();	//total length of all the queues in bytes
		int wfq_bytelength();	//total length of WFQ queues in bytes
		int prio_bytelength();	//total length of higher priority queues in bytes
		int ecn_mark(int queue_index);	//queue length ECN marking
		void tcn_mark(Packet *pkt);	//our solution: TCN

		/* Variables */
        	PacketPRIO *prio_queues;	//strict higher priority queues
        	PacketWFQ *wfq_queues;	//WFQ queues in the lowest priority

		long double currTime; //Finish time assigned to last packet
		int prio_queue_num_;    //number of higher priority queues
        	int wfq_queue_num_; //number of WFQ queues
        	int mean_pktsize_;    //MTU in bytes
        	double port_thresh_;  //per-port ECN marking threshold (pkts)
        	int marking_scheme_;  //ECN marking policy
        	double link_capacity_;    //Link capacity
        	int debug_;   //debug more(true) or not(false)

        	Tcl_Channel total_qlen_tchan_;  //Place to write total_qlen records
        	Tcl_Channel qlen_tchan_;    //Place to write per-queue qlen records
        	void trace_total_qlen();    //Routine to write total qlen records
        	void trace_qlen();  //Routine to write per-queue qlen records
};

#endif
