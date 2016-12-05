#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "flags.h"
#include "prio_dwrr.h"

#define max(arg1,arg2) (arg1>arg2 ? arg1 : arg2)
#define min(arg1,arg2) (arg1<arg2 ? arg1 : arg2)

/* Insert a queue to the tail of an active list. Return true if insert succeeds */
static void InsertTailList(PacketDWRR* list, PacketDWRR *q)
{
	if (!list || !q)
		return;

	PacketDWRR* tmp = list;
	while (true) {
		/* Arrive at the tail of this list */
		if (!(tmp->next)) {
			tmp->next = q;
			q->next = NULL;
			return;
		} else {
			tmp = tmp->next;
		}
	}
}

/* Remove and return the head node from the active list */
static PacketDWRR* RemoveHeadList(PacketDWRR* list)
{
	if (list) {
		PacketDWRR* tmp = list->next;
		if (tmp) {
			list->next = tmp->next;
			return tmp;
		} else {
			return NULL;
		}
	} else {
		return NULL;
	}
}

static class PrioDwrrClass : public TclClass
{
	public:
		PrioDwrrClass() : TclClass("Queue/PrioDwrr") {}
		TclObject* create(int argc, const char*const* argv)
		{
			return (new PRIO_DWRR);
		}
} class_prio_dwrr;

PRIO_DWRR::PRIO_DWRR()
{
	prio_queues = new PacketPRIO[MAX_PRIO_QUEUE_NUM];
	dwrr_queues = new PacketDWRR[MAX_DWRR_QUEUE_NUM];

	for (int i = 0; i < MAX_PRIO_QUEUE_NUM; i++)
		prio_queues[i].id = i;
	for (int i = 0; i < MAX_DWRR_QUEUE_NUM; i++)
		dwrr_queues[i].id = i;

	active = new PacketDWRR();

	prio_queue_num_ = 1;
	dwrr_queue_num_ = 7;

	mean_pktsize_ = 1500;
	marking_scheme_ = 0;
	port_thresh_ = 65;
	link_capacity_ = 10000000000;	// 10Gbps
	debug_ = 0;

	round_time = 0;
	last_idle_time = 0;
	mqecn_alpha_ = 0.75;
	mqecn_interval_bytes_ = 1500;

	total_qlen_tchan_ = NULL;
	qlen_tchan_ = NULL;

	/* bind variables */
	bind("prio_queue_num_", &prio_queue_num_);
	bind("dwrr_queue_num_", &dwrr_queue_num_);

	bind("mean_pktsize_", &mean_pktsize_);
	bind("marking_scheme_", &marking_scheme_);
	bind("port_thresh_", &port_thresh_);
	bind_bw("link_capacity_", &link_capacity_);
	bind_bool("debug_", &debug_);

	bind("mqecn_alpha_", &mqecn_alpha_);
	bind("mqecn_interval_bytes_", &mqecn_interval_bytes_);
}

PRIO_DWRR::~PRIO_DWRR()
{
	delete active;
	delete [] prio_queues;
	delete [] dwrr_queues;
}

/* Get total length of all DWRR queues in bytes */
int PRIO_DWRR::dwrr_bytelength()
{
	int result = 0;

	for (int i = 0; i < MAX_DWRR_QUEUE_NUM; i++)
		result += dwrr_queues[i].byteLength();

	return result;
}

/* Get total length of all higher priority queues in bytes */
int PRIO_DWRR::prio_bytelength()
{
	int result = 0;

	for (int i = 0; i < MAX_PRIO_QUEUE_NUM; i++)
		result += prio_queues[i].byteLength();

	return result;
}

/* Get total length of all queues in bytes */
int PRIO_DWRR::total_bytelength()
{
	return dwrr_bytelength() + prio_bytelength();
}


/* Determine whether we need to mark ECN. Return 1 if it requires marking */
int PRIO_DWRR::ecn_mark(int queue_index)
{
	int type = 0;
	int id = 0;

	if (queue_index < 0 || queue_index >= prio_queue_num_ + dwrr_queue_num_) {
		fprintf(stderr, "Invalid queue index value %d\n", queue_index);
		exit(1);
	}

	if (queue_index < prio_queue_num_) {
		id = queue_index;
		type = PRIO_QUEUE;
	} else {
		id = queue_index - prio_queue_num_;
		type = DWRR_QUEUE;
	}

	if (marking_scheme_ == PER_QUEUE_MARKING) {	//per-queue ECN marking
		if (type == PRIO_QUEUE &&
		    prio_queues[id].byteLength() > prio_queues[id].thresh * mean_pktsize_)
			return 1;
		else if (type == DWRR_QUEUE &&
			 dwrr_queues[id].byteLength() > dwrr_queues[id].thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	} else if (marking_scheme_ == PER_PORT_MARKING) {	//per-port ECN marking
		if (total_bytelength() > port_thresh_ * mean_pktsize_)
			return 1;
		else
			return 0;
	} else if (marking_scheme_ == MQ_ECN_MARKING) {	//MQ-ECN
		if (type == DWRR_QUEUE) {
			double thresh = port_thresh_;
			if (round_time >= 0.000000001 && link_capacity_ > 0)
				thresh = min(dwrr_queues[id].quantum * 8 / round_time / link_capacity_, 1) * port_thresh_;
			//For debug
			//printf("round time: %f threshold: %f\n",round_time, thresh);
			if (dwrr_queues[id].byteLength() > thresh * mean_pktsize_)
				return 1;
			else
				return 0;
		} else if (prio_queues[id].byteLength() > prio_queues[id].thresh * mean_pktsize_) {
			return 1;
		} else {
			return 0;
		}
	} else {
		fprintf (stderr,"Unknown ECN marking scheme %d\n", marking_scheme_);
		return 0;
	}
}

/*
 *  entry points from OTcL to set per queue state variables
 *   - $q set-quantum queue_id queue_quantum (quantum is actually weight)
 *   - $q set-thresh queue_id queue_thresh
 *   - $q attach-total file
 *   - $q attach-queue file
 *
 *  NOTE: $q represents the discipline queue variable in OTcl.
 */
int PRIO_DWRR::command(int argc, const char*const* argv)
{
	if (argc == 3) {
		int mode;
                const char* id = argv[2];
                Tcl& tcl = Tcl::instance();

		if (strcmp(argv[1], "attach-total") == 0) {   //total queue length
			total_qlen_tchan_ = Tcl_GetChannel(tcl.interp(), (char*)id, &mode);
			if (total_qlen_tchan_ == 0) {
				tcl.resultf("Cannot attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);

		} else if (strcmp(argv[1], "attach-queue") == 0) {    //per-queue queue length
			qlen_tchan_ = Tcl_GetChannel(tcl.interp(), (char*)id, &mode);
			if (qlen_tchan_ == 0) {
				tcl.resultf("Cannot attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
	} else if (argc == 4) {
		if (strcmp(argv[1], "set-quantum") == 0) {	//only for WFQ queues
			int id = atoi(argv[2]) - prio_queue_num_;
			int quantum = atoi(argv[3]);
			if (id < dwrr_queue_num_ && id >= 0 && quantum > 0) {
				dwrr_queues[id].quantum = quantum;
				return (TCL_OK);
			} else {
				fprintf(stderr, "Invalid set-quantum params: %s %s\n", argv[2], argv[3]);
				exit(1);
			}
		} else if (strcmp(argv[1], "set-thresh") == 0) {	//for all the queues
			int index = atoi(argv[2]);
			double thresh = atof(argv[3]);

			if (index < prio_queue_num_ + dwrr_queue_num_ && index >= 0 && thresh >= 0) {
				if (index < prio_queue_num_)
					prio_queues[index].thresh = thresh;
				else
					dwrr_queues[index - prio_queue_num_].thresh = thresh;
				return (TCL_OK);
			} else {
                                fprintf(stderr, "Invalid set-thresh params: %s %s\n", argv[2], argv[3]);
				exit(1);
			}
		}
	}
	return (Queue::command(argc, argv));
}

void PRIO_DWRR::reset_roundtime()
{
	if (dwrr_bytelength() > 0 || marking_scheme_ != MQ_ECN_MARKING)
		return;

	double now = Scheduler::instance().clock();
	double idle_time =  now - last_idle_time;
	if (mqecn_interval_bytes_ > 0 && link_capacity_ > 0) {
		double iter = idle_time / (mqecn_interval_bytes_ * 8 / link_capacity_);
		if ((int)iter >= 1)
			round_time = round_time * pow(mqecn_alpha_, iter);

		if(debug_) {
			double now = Scheduler::instance().clock();
			printf("%.9f round time is reset to %f after %d intervals\n", now, round_time, (int)iter);
		}
	} else {
		round_time = 0;
	}
}

/* Receive a new packet */
void PRIO_DWRR::enque(Packet *p)
{
	hdr_ip *iph = hdr_ip::access(p);
	int prio = iph->prio();
	hdr_flags* hf = hdr_flags::access(p);
	hdr_cmn* hc = hdr_cmn::access(p);
	int pktSize = hc->size();
	int qlimBytes = qlim_*mean_pktsize_;
	dwrr_queue_num_ = max(min(dwrr_queue_num_, MAX_DWRR_QUEUE_NUM), 1);
	prio_queue_num_ = max(min(prio_queue_num_, MAX_PRIO_QUEUE_NUM), 1);
	int queue_num_ = dwrr_queue_num_ + prio_queue_num_;
	double now = Scheduler::instance().clock();

	reset_roundtime();

	/* The shared buffer is overfilld */
	if (total_bytelength() + pktSize > qlimBytes) {
		drop(p);
		//printf("Packet drop\n");
		return;
	}

	if (prio >= queue_num_ || prio < 0)
		prio = queue_num_ - 1;

	if (prio < prio_queue_num_) {	//strict higher priority queues
		prio_queues[prio].enque(p);
	} else {	//WFQ queues in the lowest priority
		int id = prio - prio_queue_num_;
		dwrr_queues[id].enque(p);
		if (dwrr_queues[id].length() == 1) {
			dwrr_queues[id].deficit = dwrr_queues[id].quantum;
			dwrr_queues[id].start_time = now;
			InsertTailList(active, &dwrr_queues[id]);
		}
	}

	/* Enqueue ECN marking */
	if (marking_scheme_ != TCN_MARKING && ecn_mark(prio) > 0 && hf->ect())
		hf->ce() = 1;
	/* For TCN ,record enqueue timestamp here */
	else if (marking_scheme_ == TCN_MARKING && hf->ect())
	        hc->timestamp() = now;
}

void PRIO_DWRR::tcn_mark(Packet *pkt)
{
        if (!pkt)
                return;

        hdr_flags* hf = hdr_flags::access(pkt);
        hdr_cmn* hc = hdr_cmn::access(pkt);
        double sojourn_time = Scheduler::instance().clock() - hc->timestamp();
        double latency_thresh = 0;

        if (link_capacity_ > 0)
                latency_thresh = port_thresh_ * mean_pktsize_ * 8 / link_capacity_;

        if (hf->ect() && sojourn_time > latency_thresh) {
                hf->ce() = 1;
                if (debug_)
                        printf("sojourn time %.9f > threshold %.9f\n", sojourn_time, latency_thresh);
        }

        hc->timestamp() = 0;
}

Packet *PRIO_DWRR::deque(void)
{
	PacketDWRR *headNode = NULL;
	Packet *pkt = NULL;
	int pktSize = 0;
	double round_sample = 0;
	double now = Scheduler::instance().clock();
	int index;

	if (total_bytelength() == 0)
		return NULL;

	trace_total_qlen();
	trace_qlen();

	if (prio_bytelength() > 0) {
		for (int i = 0; i < prio_queue_num_; i++) {
			if (prio_queues[i].byteLength() > 0) {
				pkt = prio_queues[i].deque();
                                if (marking_scheme_ == TCN_MARKING)
                                        tcn_mark(pkt);

                                return pkt;
			}
		}
	}

	/* Go through all actives DWRR queues and select a packet to dequeue */
	while (1) {
		headNode = active->next;
		if (!headNode || headNode->length() == 0) {	//This should not happen!
			fprintf (stderr,"no active flow\n");
			exit(1);
		}

		pktSize = hdr_cmn::access(headNode->head())->size();
		/* if we have enough quantum to dequeue the head packet */
		if (pktSize <= headNode->deficit) {
			pkt = headNode->deque();
			headNode->deficit -= pktSize;

			/* TCN marking */
			if (marking_scheme_ == TCN_MARKING)
				tcn_mark(pkt);

			/* After dequeue, headNode becomes empty */
			if (headNode->length() == 0) {
				round_sample = now + pktSize * 8 / link_capacity_ - headNode->start_time;
				round_sample += pktSize * 8 / link_capacity_;
				round_time = round_time * mqecn_alpha_ + round_sample * (1 - mqecn_alpha_);
				headNode = RemoveHeadList(active);

				if (debug_ && marking_scheme_ == MQ_ECN_MARKING) {
					printf("sample round time: %.9f round time: %.9f\n",
					       round_sample, round_time);
				}
			}
			break;
		/* No enough quantum */
		} else {
			headNode = RemoveHeadList(active);
			headNode->deficit += headNode->quantum;
			round_sample = now - headNode->start_time;
			round_time = round_time * mqecn_alpha_ + round_sample * (1 - mqecn_alpha_);
			headNode->start_time = Scheduler::instance().clock();
			InsertTailList(active, headNode);

			if (debug_ && marking_scheme_ == MQ_ECN_MARKING) {
				printf("sample round time: %.9f round time: %.9f\n",
				       round_sample, round_time);
			}
		}
	}

	if (dwrr_bytelength() == 0)
		last_idle_time = now;

	return pkt;
}

/* routine to write total qlen records */
void PRIO_DWRR::trace_total_qlen()
{
        if (!total_qlen_tchan_)
                return;

        char wrk[100] = {0};
	sprintf(wrk, "%g, %d\n", Scheduler::instance().clock(), total_bytelength());
        Tcl_Write(total_qlen_tchan_, wrk, strlen(wrk));
}

/* routine to write per-queue qlen records */
void PRIO_DWRR::trace_qlen()
{
	if (!qlen_tchan_)
                return;

	char wrk[500] = {0};
	sprintf(wrk, "%g", Scheduler::instance().clock());
        Tcl_Write(qlen_tchan_, wrk, strlen(wrk));

	for (int i = 0; i < prio_queue_num_; i++) {
                sprintf(wrk, ", %d\0", prio_queues[i].byteLength());
		Tcl_Write(qlen_tchan_, wrk, strlen(wrk));
        }

	for (int i = 0; i < dwrr_queue_num_; i++) {
                sprintf(wrk, ", %d\0", dwrr_queues[i].byteLength());
		Tcl_Write(qlen_tchan_, wrk, strlen(wrk));
        }

	Tcl_Write(qlen_tchan_, "\n", 1);
}
