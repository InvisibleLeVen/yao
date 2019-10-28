// done
#include <thread>         
#include <chrono>        
#include <algorithm>    
#include "uni_consumer.h"
#include "pos_calcu.h"
#include "perfctx.h"
#include <tinyxml.h>
#include <tinystr.h>
#include "YaoQuote.h"


UniConsumer::UniConsumer(struct vrt_queue* queue, 
			ShfeL1MDProducer* shfeL1MDProducer, 
			ShfeFullDepthMDProducer* shfeFullDepthMDProducer,
			TunnRptProducer *tunn_rpt_producer) 
		: module_name_("uni_consumer"),
		running_(true), 
		shfeL1MDProducer_(shfeL1MDProducer), 
		shfeFullDepthMDProducer_(shfeFullDepthMDProducer),
		tunn_rpt_producer_(tunn_rpt_producer),
		lock_log_(ATOMIC_FLAG_INIT)
{
	// lic
	legal_ = check_lic();
	clog_error("[%s] legal_:%d", module_name_, legal_);

	memset(pending_signals_, -1, sizeof(pending_signals_));
	ParseConfig();
	
	log_write_count_ = 0;
	log_w_ = vector<strat_out_log>(MAX_LINES_FOR_LOG);

#ifdef PERSISTENCE_ENABLED 
    p_yao_md_save_ = new QuoteDataSave<YaoQuote>("yao_md", MY_YAO_QUOTE_TYPE);
#endif

#if FIND_STRATEGIES == 3 // strcmp
	clog_info("[%s] method for finding strategies by contract:strcmp", module_name_);
#endif

	clog_info("[%s] MAX_STRATEGY_COUNT: %d;", module_name_, MAX_STRATEGY_COUNT);

	(this->consumer_ = vrt_consumer_new(module_name_, queue));

	// lic, 非法用户，进程降级为hybrid
	if(!legal_) strcpy(config_.yield, "hybrid" );

	clog_warning("[%s] yield:%s", module_name_, config_.yield); 
	if(strcmp(config_.yield, "threaded") == 0)
	{
		this->consumer_->yield = vrt_yield_strategy_threaded();
	}
	else if(strcmp(config_.yield, "spin") == 0)
	{
		this->consumer_->yield = vrt_yield_strategy_spin_wait();
	}
	else if(strcmp(config_.yield, "hybrid") == 0)
	{
		this->consumer_->yield = vrt_yield_strategy_hybrid();
	}

	this->pproxy_ = CLoadLibraryProxy::CreateLoadLibraryProxy();
	this->pproxy_->setModuleLoadLibrary(new CModuleLoadLibraryLinux());
	this->pproxy_->setBasePathLibrary(this->pproxy_->getexedir());
	CreateStrategies();
}

UniConsumer::~UniConsumer()
{
#ifdef PERSISTENCE_ENABLED 
    if (p_yao_md_save_) delete p_yao_md_save_;
#endif

//	if (this->consumer_ != NULL){
//		vrt_consumer_free(this->consumer_);
//		this->consumer_ = NULL;
//		clog_info("[%s] release uni_consumer.", module_name_);
//	}
}

void UniConsumer::ParseConfig()
{
	std::string config_file = "x-trader.config";
	TiXmlDocument doc = TiXmlDocument(config_file.c_str());
    doc.LoadFile();
    TiXmlElement *root = doc.RootElement();    
	
	// yield strategy
    TiXmlElement *comp_node = root->FirstChildElement("Disruptor");
	if (comp_node != NULL)
	{
		strcpy(config_.yield, comp_node->Attribute("yield"));
	}
	else 
	{ 
		clog_error("[%s] x-trader.config error: Disruptor node missing.", module_name_); 
	}

    TiXmlElement *strategies_ele = root->FirstChildElement("strategies");
	if (strategies_ele != 0)
	{
		TiXmlElement *strategy_ele = strategies_ele->FirstChildElement();
		while (strategy_ele != 0)
		{
			StrategySetting strategy_setting = this->CreateStrategySetting(strategy_ele);
			this->strategy_settings_.push_back(strategy_setting);
			strategy_ele = strategy_ele->NextSiblingElement();			
		}
	}  
}

StrategySetting UniConsumer::CreateStrategySetting(const TiXmlElement *ele)
{
	StrategySetting setting;
	strcpy(setting.config.st_name, ele->Attribute("model_file"));
	setting.config.st_id = stoi(ele->Attribute("id"));
	setting.file = ele->Attribute("model_file");
	// log_id
	setting.config.log_id = setting.config.st_id *10 + 0;
	// log_name
	strcpy(setting.config.log_name, ele->Attribute("log_name"));
	// iv_id
	setting.config.iv_id = setting.config.st_id *10 + 1;
	// iv_name
	strcpy(setting.config.iv_name ,ele->Attribute("iv_name"));
	// ev_id
	setting.config.ev_id = setting.config.st_id *10 + 2;
	// ev_name
	const char * name = ele->Attribute("ev_name");
	strcpy(setting.config.ev_name, ele->Attribute("ev_name"));

	int counter = 0;
	const TiXmlElement* symbol_ele = ele->FirstChildElement();		
	while (symbol_ele != 0)	
	{
		symbol_t &tmp = setting.config.symbols[counter];

		symbol_ele->QueryIntAttribute("max_pos", &tmp.max_pos);
		strcpy(tmp.name, symbol_ele->Attribute("name"));
		string exc_str = symbol_ele->Attribute("exchange");
		tmp.exchange = static_cast<exchange_names>(exc_str[0]);
		symbol_ele->QueryIntAttribute("symbol_log_id",&tmp.symbol_log_id);
		strcpy(tmp.symbol_log_name, symbol_ele->Attribute("symbol_log_name"));

		symbol_ele = symbol_ele->NextSiblingElement();
		counter++;
	}	//end while (symbol_ele != 0)
	setting.config.symbols_cnt = counter;
	   
	return setting;
}

void UniConsumer::CreateStrategies()
{
	strategy_counter_ = 0;
	for (auto &setting : this->strategy_settings_){
		Strategy &strategy = stra_table_[strategy_counter_];

		// TODO: yao
		seting.config.TradingDay = this->tunn_rpt_producer_->TradingDay;
		seting.config.IsNightTrading = this->tunn_rpt_producer_->IsNightTrading;

		strategy.Init(setting, this->pproxy_);
		// mapping table
		straid_straidx_map_table_[setting.config.st_id] = strategy_counter_ ;
		// strategy log
		FILE *log_file = strategy.get_log_file();
		WriteLogTitle(log_file);
		WriteStrategyLog(strategy);

		// TODO: 需要支持一个策略交易多个合约
		// TODO: yao
		clog_info("[%s] [CreateStrategies] id:%d;  so:%s ", 
					module_name_,
					stra_table_[strategy_counter_].GetId(),
					stra_table_[strategy_counter_].GetSoFile());

		strategy_counter_++;
	}
}

void UniConsumer::ProcYaoQuote(YaoQuote* md)
{
	//clog_info("[test] proc [%s] [ProcShfeMarketData] contract:%s, time:%s", module_name_, 
	//	md->InstrumentID, md->GetQuoteTime().c_str());

	for(int i = 0; i < strategy_counter_; i++)
	{ 
		int sig_cnt = 0;
		Strategy &strategy = stra_table_[i];

		if (strategy.Subscribed(md->Contract))
		{
			strategy.FeedMd(md, &sig_cnt, sig_buffer_);
			WriteStrategyLog(strategy);
			ProcSigs(strategy, sig_cnt, sig_buffer_);
		}
	}

#ifdef PERSISTENCE_ENABLED 
    timeval t;
    gettimeofday(&t, NULL);
    p_yao_md_save_->OnQuoteData(t.tv_sec * 1000000 + t.tv_usec, md);
#endif
}
void UniConsumer::Start()
{
	running_  = true;
	// strategy log
	thread_log_ = new std::thread(&UniConsumer::WriteLogImp,this);

	MYQuoteData myquotedata(shfeFullDepthMDProducer_, shfeL1MDProducer_);
	auto f_shfemarketdata = std::bind(&UniConsumer::ProcYaoQuote, this,_1);
	myquotedata.SetQuoteDataHandler(f_shfemarketdata);

	// INE sc 
	MYIneQuoteData myinequotedata(shfeFullDepthMDProducer_, shfeL1MDProducer_);
	auto f_inemarketdata = std::bind(&UniConsumer::ProcYaoQuote, this,_1);
	myinequotedata.SetQuoteDataHandler(f_inemarketdata);

	int rc = 0;
	struct vrt_value  *vvalue;
	while (running_ &&
		   (rc = vrt_consumer_next(consumer_, &vvalue)) != VRT_QUEUE_EOF) 
	{
		if (rc == 0) 
		{
			struct vrt_hybrid_value *ivalue = 
				cork_container_of(vvalue, struct vrt_hybrid_value, parent);
			switch (ivalue->data)
			{
				case INE_L1_MD:
					myinequotedata.ProcIneL1MdData(ivalue->index);
				case SHFE_L1_MD:
					myquotedata.ProcShfeL1MdData(ivalue->index);
					break;
				// 解决原油(SC)因序号与上期其它品种的序号是独立的，从而造成数据问题。
				// 解决方法：将sc与其它品种行情分成2种独立行情
				case INE_FULL_DEPTH_MD:
					myinequotedata.ProcIneFullDepthData(ivalue->index);
					break;
				case SHFE_FULL_DEPTH_MD:
					myquotedata.ProcShfeFullDepthData(ivalue->index);
					break;
				case DCE_YAO_DATA:
					ProcDceYaoData(ivalue->index);
					break;
				case ZCE_YAO_DATA:
					ProcZceYaoData(ivalue->index);
					break;
				case TUNN_RPT:
					ProcTunnRpt(ivalue->index);
					break;
				default:
					clog_info("[%s] [start] unexpected index: %d", 
								module_name_, 
								ivalue->index);
					break;
			}
		}
	} // end while (running_ &&

	if (rc == VRT_QUEUE_EOF) 
	{
		clog_info("[%s] [start] rev EOF.", module_name_);
	}
	clog_info("[%s] [start] start exit.", module_name_);
}

void UniConsumer::Stop()
{
	if(running_)
	{
		shfeL1MDProducer_->End();
		shfeFullDepthMDProducer_->End();
		tunn_rpt_producer_->End();

#ifdef COMPLIANCE_CHECK
			compliance_.Save();
#endif

		running_ = false;
		thread_log_ ->join();
		FlushStrategyLog();
		for(int i=0; i<strategy_counter_; i++)
		{
			stra_table_[i].End();
		}

		if (pproxy_ != NULL)
		{
			pproxy_->DeleteLoadLibraryProxy();
			pproxy_ = NULL;
		}

		clog_warning("[%s] End exit", module_name_);
	}
	fflush (Log::fp);
}

void UniConsumer::ProcDceYaoData(int32_t index)
{
	// TODO: code here
//	YaoQuote* md = md_producer_->Data(index);
//	ProcYaoQuote(md);

}

void UniConsumer::ProcZceYaoData(int32_t index)
{
	// TODO: code here
//	YaoQuote* md = md_producer_->Data(index);
//	ProcYaoQuote(md);

}

void UniConsumer::ProcTunnRpt(int32_t index)
{
#ifdef LATENCY_MEASURE
		high_resolution_clock::time_point t0 = high_resolution_clock::now();
#endif
	int sig_cnt = 0;

	TunnRpt* rpt = tunn_rpt_producer_->GetRpt(index);
	int32_t strategy_id = tunn_rpt_producer_->GetStrategyID(*rpt);

	clog_info("[%s] [ProcTunnRpt] index: %d; LocalOrderID: %d; OrderStatus:%c; "
		"MatchedAmount:%d; ErrorID:%d ",
		module_name_, 
		index, 
		rpt->LocalOrderID, 
		rpt->OrderStatus, 
		rpt->MatchedAmount,
		rpt->ErrorID);

	Strategy& strategy = stra_table_[straid_straidx_map_table_[strategy_id]];
	int32_t sigidx = strategy.GetSignalIdxByLocalOrdId(rpt->LocalOrderID);
	const char* contract = strategy.GetContractBySigIdx(sig_idx);
	strategy.FeedTunnRpt(sigidx, *rpt, &sig_cnt, sig_buffer_);
	WriteStrategyLog(strategy);

	// TODO: to here
#ifdef COMPLIANCE_CHECK
	// 拒绝也是THOST_FTDC_OST_Canceled状态，因而可能会造成多计算撤单次数，但不会产生大问题
	if (rpt->OrderStatus == THOST_FTDC_OST_PartTradedNotQueueing ||
			rpt->OrderStatus == THOST_FTDC_OST_NoTradeNotQueueing ||
			rpt->OrderStatus == THOST_FTDC_OST_Canceled)
	{
		compliance_.AccumulateCancelTimes(contract);
	}

	if (tunn_rpt_producer_->IsFinal(rpt->OrderStatus))
	{
		int32_t counter = strategy.GetCounterByLocalOrderID(rpt->LocalOrderID);
		compliance_.End(counter);
	}
#endif
	ProcSigs(strategy, sig_cnt, sig_buffer_);

#ifdef LATENCY_MEASURE
		high_resolution_clock::time_point t1 = high_resolution_clock::now();
		int latency = (t1.time_since_epoch().count() - t0.time_since_epoch().count()) / 1000;
		clog_warning("[%s] ProcTunnRpt latency:%d us", module_name_, latency); 
#endif
}

void UniConsumer::ProcSigs(Strategy &strategy, 
			int32_t sig_cnt, 
			signal_t *sigs)
{
	clog_info("[%s] [ProcSigs] sig_cnt: %d; ", 
				module_name_, 
				sig_cnt);

	for (int i = 0; i < sig_cnt; i++)
	{
		if (sigs[i].sig_act == signal_act_t::cancel)
		{
			CancelOrder(strategy, sigs[i]);
		}
		else
		{
			signal_t &sig = sigs[i];
			strategy.Push(sig);
			PlaceOrder(strategy, sigs[i]); 
		}
	}
}

void UniConsumer::CancelOrder(Strategy &strategy,signal_t &sig)
{
	// improve place, cancel
	int32_t ori_sigid = sig.orig_sig_id;
	int orderRef = tunn_rpt_producer_->NewLocalOrderID(strategy.GetId());
	signal_t *orig_sig = strategy.GetSignalBySigID(ori_sigid);
	int orig_orderRef = strategy.GetLocalOrderID(sig.orig_sig_id); // 即LocalOrderId
	char* orig_sysOrderId = strategy.GetSysOrderIdBySigID(sig.orig_sig_id); 
	CThostFtdcInputOrderActionField* orderAction = 
		CtpFieldConverter::Convert( orig_sig->exchange, 
					orig_sig->symbol, 
					orderRef, 
					orig_orderRef, 
					orig_sysOrderId);    		  	
	this->tunn_rpt_producer_->ReqOrderAction(orderAction);

#ifdef LATENCY_MEASURE
		int latency = perf_ctx::calcu_latency(sig.st_id, sig.sig_id);
        if(latency > 0) clog_warning("[%s] cancel latency:%d us", module_name_, latency); 
#endif
}

void UniConsumer::PlaceOrder(Strategy &strategy,const signal_t &sig)
{
	int vol = strategy.GetVol(sig);
	int localorderid = tunn_rpt_producer_->NewLocalOrderID(strategy.GetId());
	strategy.PrepareForExecutingSig(localorderid, sig, vol);

	CThostFtdcInputOrderField *ord = 
		CtpFieldConverter::Convert( sig, localorderid, vol);

#ifdef COMPLIANCE_CHECK
	int32_t counter = strategy.GetCounterByLocalOrderID(localorderid);
	bool result = compliance_.TryReqOrderInsert(counter, 
				ord->InstrumentID, 
				ord->LimitPrice,
				ord->Direction, 
				ord->CombOffsetFlag[0]);
	if(result)
	{
#endif
		int32_t rtn = tunn_rpt_producer_->ReqOrderInsert(ord);
		if(rtn != 0)
		{ // feed rejeted info
			TunnRpt rpt;
			memset(&rpt, 0, sizeof(rpt));
			rpt.LocalOrderID = localorderid;
			rpt.OrderStatus = THOST_FTDC_OST_Canceled;
			rpt.ErrorID = rtn;

			compliance_.End(counter);

			int sig_cnt = 0;
			int32_t sigidx = strategy.GetSignalIdxByLocalOrdId(localorderid);
			strategy.FeedTunnRpt(sigidx, rpt, &sig_cnt, sig_buffer_);
			WriteStrategyLog(strategy);
			ProcSigs(strategy, sig_cnt, sig_buffer_);
		}
#ifdef COMPLIANCE_CHECK
	}
	else
	{
		 clog_error("[%s] compliance checking failed:%ld", 
					 module_name_,
					 localorderid);
		 clog_error("[%s] strategy id:%d; compliance checking failed, "
					 "signal: strategy id:%d; sig_id:%d; exchange:%d; "
					 "symbol:%s; open_volume:%d; buy_price:%f; "
					 "close_volume:%d; sell_price:%f; sig_act:%d; "
					 "sig_openclose:%d; orig_sig_id:%d", 
					 module_name_,
					 strategy.GetId(), 
					 sig.st_id, 
					 sig.sig_id, 
					 sig.exchange, 
					 sig.symbol, 
					 sig.open_volume, 
					 sig.buy_price, 
					 sig.close_volume, 
					 sig.sell_price, 
					 sig.sig_act, 
					 sig.sig_openclose, 
					 sig.orig_sig_id); 

		// feed rejeted info
		TunnRpt rpt;
		memset(&rpt, 0, sizeof(rpt));
		rpt.LocalOrderID = localorderid;
		rpt.OrderStatus = THOST_FTDC_OST_Canceled;
		rpt.ErrorID = 5;
		int sig_cnt = 0;
		int32_t sigidx = strategy.GetSignalIdxByLocalOrdId(rpt.LocalOrderID);
		strategy.FeedTunnRpt(sigidx, rpt, &sig_cnt, sig_buffer_);
		WriteStrategyLog(strategy);
		ProcSigs(strategy, sig_cnt, sig_buffer_);
	}
#endif

#ifdef LATENCY_MEASURE
  // latency measure
	int latency = perf_ctx::calcu_latency(sig.st_id, sig.sig_id);
	if(latency > 0) clog_warning("[%s] place latency:%d us", module_name_, latency); 
#endif
}

// 遍历策略，将缓存日志写到文件中
void UniConsumer::FlushStrategyLog()
{
	for(int i = 0; i < strategy_counter_; i++)
	{ 
		Strategy &strategy = stra_table_[i];
		pfDayLogFile_ = strategy.get_log_file();
		strategy.get_log(log_w_, log_write_count_);
		for(int i = 0; i < log_write_count_; i++)
		{
			WriteOne(pfDayLogFile_, log_w_.data()+i);
		}
	} // end for(int i = 0; i < strategy_counter_; i++) 
}
	
void UniConsumer::WriteLogTitle(FILE * pfDayLogFile)
{
	// title
	fprintf (pfDayLogFile, "exch_time  contract  n_tick  price  vol  bv1  bp1  sp1  sv1  amt  ");
	fprintf (pfDayLogFile, "oi buy_price  sell_price  open_vol  close_vol  ");
	fprintf (pfDayLogFile, "long_pos  short_pos  total_ordervol  total_cancelvol order_count cancel_count ");
	fprintf (pfDayLogFile, "cash live total_vol max_dd max_net_pos max_side_pos ");
	for(int i=0; i< 11; i++)
	{
		fprintf(pfDayLogFile,"sig%d ", i);
	}
	fprintf(pfDayLogFile,"sig11\n");
}

void UniConsumer::WriteLogImp()
{	
	while(true)
	{
		while (lock_log_.test_and_set()) { }


		for(int i = 0; i < log_write_count_; i++)
		{
			WriteOne(pfDayLogFile_, log_w_.data()+i);
		}
		log_write_count_ = 0;

		if(!running_)
		{
			lock_log_.clear();
			break;
		}
		lock_log_.clear();

		std::this_thread::sleep_for (std::chrono::milliseconds(10));
	} // end while(true)
	clog_warning("[%s] WriteLogImp exit", module_name_); 
}

void UniConsumer::WriteOne(FILE *pfDayLogFile, struct strat_out_log *pstratlog)
{
    fprintf(pfDayLogFile,"%d %6s %d %14.2f %d ",
            pstratlog->exch_time,
            pstratlog->contract,
            pstratlog->n_tick,
            pstratlog->price,
            pstratlog->vol);

    fprintf(pfDayLogFile,"%d %12.4f %12.4f %d %ld %ld ",
            pstratlog->bv1,
            pstratlog->bp1,
            pstratlog->sp1,
            pstratlog->sv1,
            pstratlog->amt,
            pstratlog->oi);

    fprintf(pfDayLogFile,"%12.4f %12.4f %d %d ",
            pstratlog->buy_price,
            pstratlog->sell_price,
            pstratlog->open_vol,
            pstratlog->close_vol);

    fprintf(pfDayLogFile,"%d %d %d %d %d %d ",
            pstratlog->long_pos,
            pstratlog->short_pos,
            pstratlog->tot_ordervol,
            pstratlog->tot_cancelvol,
            pstratlog->order_cnt,
            pstratlog->cancel_cnt);

    fprintf(pfDayLogFile,"%16.2f %16.2f %d %16.2f %d %d ",
            pstratlog->cash,
            pstratlog->live,
            pstratlog->tot_vol,
            pstratlog->max_dd,
            pstratlog->max_net_pos,
            pstratlog->max_side_pos);

    for(int i=0; i< 11; i++)
    {
        fprintf(pfDayLogFile,"%0.2f ", pstratlog->sig[i]);
    }
    fprintf(pfDayLogFile,"%0.2f\n", pstratlog->sig[11]);
}

void UniConsumer::WriteStrategyLog(Strategy &strategy)
{
	if(strategy.IsLogFull())
	{
#ifdef LATENCY_MEASURE
		high_resolution_clock::time_point t0 = high_resolution_clock::now();
#endif
		// 在日志写线程睡眠时，日志缓存可能会被覆盖
		while (lock_log_.test_and_set()) {}
		pfDayLogFile_ = strategy.get_log_file();
		strategy.get_log(log_w_, log_write_count_);

		clog_info("[%s] WriteStrategyLog strategy:%d; log_write_count_:%d;pfDayLogFile_:%d ", 
			module_name_,strategy.GetId(), log_write_count_, pfDayLogFile_ ); 

		lock_log_.clear();

#ifdef LATENCY_MEASURE
		high_resolution_clock::time_point t1 = high_resolution_clock::now();
		int latency = (t1.time_since_epoch().count() - t0.time_since_epoch().count()) / 1000;
		clog_warning("[%s] WriteStrategyLog latency:%d us", module_name_, latency); 
#endif
	} // end if(strategy.IsLogFull())
}


// lic
bool UniConsumer::check_lic()
{
	bool legal = false;
	char target[1024];

	getcwd(target, sizeof(target));
	string content = target;
	if(content.find("u910019")==string::npos)
	{
		legal = false;
	}
	else
	{
		legal = true;
	}
	clog_warning("[%s] check:%d", module_name_, legal); 
	return legal;
}




