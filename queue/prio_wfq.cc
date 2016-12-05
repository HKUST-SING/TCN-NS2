#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "flags.h"
#include "prio_wfq.h"

#define max(arg1,arg2) (arg1>arg2 ? arg1 : arg2)
#define min(arg1,arg2) (arg1<arg2 ? arg1 : arg2)

static class PrioWfqClass : public TclClass
{
    public:
        PrioWfqClass() : TclClass("Queue/PrioWfq") {}
        TclObject* create(int argc, const char*const* argv)
        {
			return (new PRIO_WFQ);
        }
} class_prio_wfq;

PRIO_WFQ::PRIO_WFQ()
{
        prio_queues = new PacketPRIO[MAX_PRIO_QUEUE_NUM];
        wfq_queues = new PacketWFQ[MAX_WFQ_QUEUE_NUM];

        currTime = 0;
        prio_queue_num_ = 1;
	wfq_queue_num_ = 7;
	mean_pktsize_ = 1500;
	port_thresh_ = 65;
	marking_scheme_ = PER_QUEUE_MARKING;
	link_capacity_ = 10000000000;  //10Gbps
	debug_ = 0;

	total_qlen_tchan_ = NULL;
	qlen_tchan_ = NULL;

	/* bind variables */
        bind("prio_queue_num_", &prio_queue_num_);
        bind("wfq_queue_num_", &wfq_queue_num_);
	bind("mean_pktsize_", &mean_pktsize_);
	bind("port_thresh_", &port_thresh_);
	bind("marking_scheme_", &marking_scheme_);
	bind_bw("link_capacity_", &link_capacity_);
	bind_bool("debug_", &debug_);
}

PRIO_WFQ::~PRIO_WFQ()
{
        delete [] prio_queues;
        delete [] wfq_queues;
}

/* Get total length of all WFQ queues in bytes */
int PRIO_WFQ::wfq_bytelength()
{
	int result = 0;

	for (int i = 0; i < MAX_WFQ_QUEUE_NUM; i++)
		result += wfq_queues[i].byteLength();

	return result;
}

/* Get total length of all higher priority queues in bytes */
int PRIO_WFQ::prio_bytelength()
{
	int result = 0;

	for (int i = 0; i < MAX_PRIO_QUEUE_NUM; i++)
		result += prio_queues[i].byteLength();

	return result;
}

/* Get total length of all queues in bytes */
int PRIO_WFQ::total_bytelength()
{
	return wfq_bytelength() + prio_bytelength();
}

/*
 * queue-length ECN marking
 * Return 1 if it the packet should gets marked
 */
int PRIO_WFQ::ecn_mark(int queue_index)
{
	int type = 0, index = 0;

	if (queue_index < 0 || queue_index >= prio_queue_num_ + wfq_queue_num_) {
		fprintf(stderr, "Invalid queue index value %d\n", queue_index);
		exit(1);
	}

	if (queue_index < prio_queue_num_) {
		index = queue_index;
		type = PRIO_QUEUE;
	} else {
                index = queue_index - prio_queue_num_;
                type = WFQ_QUEUE;
	}

	if (marking_scheme_ == PER_QUEUE_MARKING) {    //per-queue marking
		if (type == PRIO_QUEUE &&
                    prio_queues[index].byteLength() > prio_queues[index].thresh * mean_pktsize_)
			return 1;
		else if (type == WFQ_QUEUE &&
                         wfq_queues[index].byteLength() > wfq_queues[index].thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	} else if (marking_scheme_ == PER_PORT_MARKING) {      //per-port marking
		if (total_bytelength() > port_thresh_ * mean_pktsize_)
			return 1;
		else
			return 0;
	} else {
		fprintf(stderr, "Unknown ECN marking scheme %d\n", marking_scheme_);
		return 0;
	}
}

/*
 *  entry points from OTcL to set per queue state variables
 *  - $q set-weight queue_id queue_weight
 *  - $q set-thresh queue_id queue_thresh
 *  - $q attach-total file
 *  - $q attach-queue file
 *
 *  NOTE: $q represents the discipline queue variable in OTcl.
 */
int PRIO_WFQ::command(int argc, const char*const* argv)
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

		if (strcmp(argv[1], "set-weight") == 0) {     //only for WFQ queues
			int index = atoi(argv[2]) - prio_queue_num_; //index of WFQ
                        int weight = atoi(argv[3]);     //WFQ queue weight
			if (index < wfq_queue_num_ && index >= 0 && weight > 0) {
                                wfq_queues[index].weight = weight;
				return (TCL_OK);
			} else {
				fprintf(stderr, "Invalid set-weight params: %s %s\n", argv[2], argv[3]);
				exit(1);
			}

		} else if (strcmp(argv[1], "set-thresh") == 0) {      //for all the queues
			int index = atoi(argv[2]);
			double thresh = atof(argv[3]);
			if (index < prio_queue_num_ + wfq_queue_num_ && index >= 0 && thresh >= 0) {
                                if (index < prio_queue_num_)
                                        prio_queues[index].thresh = thresh;
				else
					wfq_queues[index - prio_queue_num_].thresh = thresh;
				return (TCL_OK);

			} else {
                                fprintf(stderr, "Invalid set-thresh params: %s %s\n", argv[2], argv[3]);
				exit(1);
			}
		}
	}
	return (Queue::command(argc, argv));
}

/* Receive a new packet */
void PRIO_WFQ::enque(Packet *p)
{
	hdr_ip *iph = hdr_ip::access(p);
	int prio = iph->prio();
	hdr_flags* hf = hdr_flags::access(p);
	hdr_cmn* hc = hdr_cmn::access(p);
	int pktSize = hc->size();
	int qlimBytes = qlim_ * mean_pktsize_;
        wfq_queue_num_ = max(min(wfq_queue_num_, MAX_WFQ_QUEUE_NUM), 1);
	prio_queue_num_ = max(min(prio_queue_num_, MAX_PRIO_QUEUE_NUM), 1);
	int queue_num_ = wfq_queue_num_ + prio_queue_num_;

	/* the shared buffer is overfilld */
	if (total_bytelength() + pktSize > qlimBytes) {
		drop(p);
		//printf("Packet drop\n");
		return;
	}

	if (prio >= queue_num_ || prio < 0)
	       prio = queue_num_ - 1;

        if (prio < prio_queue_num_) {   //strict higher priority queues
                prio_queues[prio].enque(p);
        } else {        //WFQ queues in the lowest priority
                int index = prio - prio_queue_num_;
                /* if the queue is empty, calculate headFinishTime and currTime */
                if (wfq_queues[index].length() == 0 && wfq_queues[index].weight > 0) {
                        wfq_queues[index].headFinishTime = currTime + pktSize / wfq_queues[index].weight ;
                        currTime = wfq_queues[index].headFinishTime;
                } else if (wfq_queues[index].weight == 0) {
                        fprintf(stderr,"Invalid weight value %f for queue %d\n", wfq_queues[index].weight, prio);
                        exit(1);
                }
                wfq_queues[index].enque(p);
        }

        /* Enqueue ECN marking */
        if (marking_scheme_ != TCN_MARKING && ecn_mark(prio) > 0 && hf->ect())
		hf->ce() = 1;
        /* For TCN ,record enqueue timestamp here */
        else if (marking_scheme_ == TCN_MARKING && hf->ect())
                hc->timestamp() = Scheduler::instance().clock();
}

void PRIO_WFQ::tcn_mark(Packet *pkt)
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

Packet* PRIO_WFQ::deque(void)
{
	Packet *pkt = NULL, *nextPkt = NULL;
	long double minT = LDBL_MAX ;
	int queue = -1;

        if (prio_bytelength() > 0) {
                for (int i = 0; i < prio_queue_num_; i++) {
			if (prio_queues[i].byteLength() > 0) {
				pkt = prio_queues[i].deque();
                                if (marking_scheme_ == TCN_MARKING)
                                        tcn_mark(pkt);
                                break;
			}
		}
        } else if (wfq_bytelength() > 0) {
		/* look for the candidate queue with the earliest virtual finish time */
		for (int i = 0; i < wfq_queue_num_; i++) {
			if (wfq_queues[i].length() > 0 && wfq_queues[i].headFinishTime < minT) {
				queue = i;
				minT = wfq_queues[i].headFinishTime;
			}
		}

		if (queue == -1 && wfq_bytelength() > 0) {
			fprintf(stderr,"not work conserving\n");
			exit(1);
		}

		pkt = wfq_queues[queue].deque();
                if (marking_scheme_ == TCN_MARKING)
                        tcn_mark(pkt);

		/* Set the headFinishTime for the remaining head packet in the queue */
		nextPkt = wfq_queues[queue].head();
		if (nextPkt && wfq_queues[queue].weight > 0) {
                        wfq_queues[queue].headFinishTime = wfq_queues[queue].headFinishTime + \
                        (hdr_cmn::access(nextPkt)->size()) / wfq_queues[queue].weight;
			if (currTime < wfq_queues[queue].headFinishTime)
				currTime = wfq_queues[queue].headFinishTime;
		} else if (!nextPkt) {        //the queue becomes empty
			wfq_queues[queue].headFinishTime = LDBL_MAX;
		}
	}

        trace_total_qlen();
        trace_qlen();

	return pkt;
}

/* routine to write total qlen records */
void PRIO_WFQ::trace_total_qlen()
{
        if (!total_qlen_tchan_)
                return;

        char wrk[100] = {0};
	sprintf(wrk, "%g, %d\n", Scheduler::instance().clock(), total_bytelength());
        Tcl_Write(total_qlen_tchan_, wrk, strlen(wrk));
}

/* routine to write per-queue qlen records */
void PRIO_WFQ::trace_qlen()
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

	for (int i = 0; i < wfq_queue_num_; i++) {
                sprintf(wrk, ", %d\0", wfq_queues[i].byteLength());
		Tcl_Write(qlen_tchan_, wrk, strlen(wrk));
        }

	Tcl_Write(qlen_tchan_, "\n", 1);
}
