/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_KV_APP_H_
#define PS_KV_APP_H_
#include <algorithm>
#include <utility>
#include <vector>
#include <unordered_map>
#include "ps/base.h"
#include "ps/simple_app.h"
#include <unistd.h>
#include "ps/internal/message.h"
#include <zmq.h>
#include <time.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <random>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../../src/resender.h"

#ifndef LITTLE_GRAIN_MSG
#define LITTLE_GRAIN_MSG
#endif

#ifndef EVAL_CONTRIBUTE_N
#define EVAL_CONTRIBUTE_N
#endif
#ifndef EVAL_CONTRIBUTE_LOSS
#define EVAL_CONTRIBUTE_LOSS
#endif
#ifndef EVAL_CONTRIBUTE_CON
#define EVAL_CONTRIBUTE_CON
#endif
#ifndef SEND_RANDOM_DROP
#define SEND_RANDOM_DROP
#endif
#ifndef ADAPTIVE_K
#define ADAPTIVE_K
#endif
namespace ps {

/**
 * \brief the structure for a list of key-value pairs
 *
 * The keys must be unique and sorted in an increasing order.  The length of a
 * value can be more than one. If \a lens is empty, then the length
 * of a value is determined by `k=vals.size()/keys.size()`.  The \a i-th KV pair
 * is then
 *
 * \verbatim {keys[i], (vals[i*k], ..., vals[(i+1)*k-1])} \endverbatim
 *
 * If \a lens is given, then `lens[i]` is the length of the \a i-th
 * value. Let
 *
 * \verbatim n = lens[0] + .. + lens[i-1]  \endverbatim
 *
 * then the \a i-th KV pair is presented as
 *
 * \verbatim {keys[i], (vals[n], ..., vals[lens[i]+n-1])} \endverbatim
 */
template <typename Val>
struct KVPairs {
  // /** \brief empty constructor */
  // KVPairs() {}
  /** \brief the list of keys */
  SArray<Key> keys;
  /** \brief the according values */
  SArray<Val> vals;
  /** \brief the according value lengths (could be empty) */
  SArray<int> lens;
  /** \brief priority */
  int priority = 0;
};

/**
 * \brief A worker node that can \ref Push (\ref Pull) key-value pairs to (from) server
 * nodes
 *
 * \tparam Val the type of value, which should be primitive types such as
 * int32_t and float
 */
template<typename Val>
class KVWorker : public SimpleApp {
 public:
  /** avoid too many this-> */
  using SimpleApp::obj_;
  /**
   * \brief callback function for \ref Push and \ref Pull
   *
   * It is called by the data receiving thread of this instance when the push or
   * pull is actually finished. Namely the kv pairs have already written into
   * servers' data structure or the kv pairs have already pulled back.
   */
  using Callback = std::function<void()>;

  /**
   * \brief constructor
   *
   * \param app_id the app id, should match with \ref KVServer's id
   * \param customer_id the customer id which is unique locally
   */
  explicit KVWorker(int app_id, int customer_id) : SimpleApp() {
    using namespace std::placeholders;
    slicer_ = std::bind(&KVWorker<Val>::DefaultSlicer, this, _1, _2, _3);
    obj_ = new Customer(app_id, customer_id, std::bind(&KVWorker<Val>::Process, this, _1));
      contri_alpha = dmlc::GetEnv("DGT_CONTRI_ALPHA", 0.3);
//      std::cout << "node-1 contri_alpha = " << contri_alpha << std::endl;
      set_random = dmlc::GetEnv("DGT_SET_RANDOM", 0);
      dgt_info = dmlc::GetEnv("DGT_INFO", 0);
      enable_block = dmlc::GetEnv("DGT_ENABLE_BLOCK", 0);
      block_size = dmlc::GetEnv("DGT_BLOCK_SIZE", 0);
      test_block_size = block_size;
      enable_dgt = dmlc::GetEnv("ENABLE_DGT", 0);
      clear_zero = dmlc::GetEnv("CLEAR_ZERO", 0); //
//      std::cout << "node-1 set_random = " << set_random << " dgt_info = "<<dgt_info<< " enable_block = " << enable_block<<" block_size = " << block_size << " enable_dgt = "<< enable_dgt << std::endl;
//init_dgt();
  }

  /** \brief deconstructor */
  virtual ~KVWorker() { delete obj_; obj_ = nullptr; }

  /**
   * \brief Pushes a list of key-value pairs to all server nodes.
   *
   * This function pushes a KV list specified by \a keys and \a vals to all
   * server nodes.
   *
   * Sample usage: the following codes push two KV pairs `{1, (1.1, 1.2)}` and `{3,
   * (3.1,3.2)}` to server nodes, where the value is a length-2 float vector
   * \code
   *   KVWorker<float> w;
   *   std::vector<Key> keys = {1, 3};
   *   std::vector<float> vals = {1.1, 1.2, 3.1, 3.2};
   *   w.Push(keys, vals);
   * \endcode
   *
   * If \a lens is given, then the value can be various length. See
   * \ref KVPairs for more information.
   *
   * The KV list is partitioned and sent based on the key range each server
   * maintaining. This function returns without waiting the data are sent
   * actually. Instead, use either \ref Wait or the callback to know when
   * finished. This function is thread-safe.
   *
   * @param keys a list of keys, must be unique and sorted in increasing order
   * @param vals the according values
   * @param lens optional, lens[i] stores the value length of the \a
   * i-th KV pair
   * @param cmd an optional command sent to the servers
   * @param cb the callback which is called when the push is finished.
   * @return the timestamp of this request
   */
  int Push(const std::vector<Key>& keys,
           const std::vector<Val>& vals,
           const std::vector<int>& lens = {},
           int cmd = 0,
           const Callback& cb = nullptr,
           int priority = 0) {
    return ZPush(
        SArray<Key>(keys), SArray<Val>(vals), SArray<int>(lens), cmd, cb,
        priority);
  }

  /**
   * \brief Pulls the values associated with the keys from the server nodes
   *
   * This function pulls the values of the keys specified in \a keys from the
   * server nodes. The format is same to \ref KVPairs
   *
   * Sample usage: the following codes pull the values of keys \a 1 and \a 3
   * from the server nodes.
   * \code
   *   KVWorker<float> w;
   *   std::vector<Key> keys = {1, 3};
   *   std::vector<float> vals;
   *   w.Pull(keys, &vals);
   * \endcode
   *
   * It's a non-blocking call. The actual pulling is finished,
   * namely \a vals (and \a lens) is filled with pulled values, only
   * if \ref Wait returns or the callback is called.
   *
   * @param keys a list of keys, must be unique and sorted in increasing order
   * @param vals the buffer for the pulled values. It can be 0 size.
   * @param lens optional buffer for the value length. If set, it can be 0 size.
   * @param cmd an optional command sent to the servers
   * @param cb the callback which is called when the pull is finished.
   * @return the timestamp of this request
   */
  int Pull(const std::vector<Key>& keys,
           std::vector<Val>* vals,
           std::vector<int>* lens = nullptr,
           int cmd = 0,
           const Callback& cb = nullptr,
           int priority = 0) {
//	  std::cout<<"Node-1 Pull!"<<std::endl;
    SArray<Key> skeys(keys);
    int ts = AddPullCB(skeys, vals, lens, cmd, cb);
    KVPairs<Val> kvs;
    kvs.keys = skeys;
    kvs.priority = priority;
    Send(ts, false, true, cmd, kvs);
    return ts;
  }

  /**
   * \brief Pushes and Pulls a list of key-value pairs to and from the server
   * nodes.
   *
   * This function pushes the values of the keys specified in \a keys to the
   * server nodes and subsequently pulls and updates the values in \a vals.
   *
   * Sample usage: the following code pushes and pulls the values of keys
   * \a 1 and \a 3 to and from the server nodes.
   * \code
   *   KVWorker<float> w;
   *   std::vector<Key> keys = {1, 3};
   *   std::vector<float> vals;
   *   w.PushPull(keys, &vals);
   * \endcode
   *
   * It's a non-blocking call. The actual pulling is finished,
   * namely \a vals (and \a lens) is filled with pulled values, only
   * if \ref Wait returns or the callback is called.
   *
   * @param keys a list of keys, must be unique and sorted in increasing order
   * @param vals the according values
   * @param outs the buffer for the pulled values. It can be 0 size.
   * @param lens optional buffer for the value length. If set, it can be 0 size.
   * @param cmd an optional command sent to the servers
   * @param cb the callback which is called when the pull is finished.
   * @return the timestamp of this request
   */
  int PushPull(const std::vector<Key>& keys,
               const std::vector<Val>& vals,
               std::vector<Val>* outs,
               std::vector<int>* lens = nullptr,
               int cmd = 0,
               const Callback& cb = nullptr,
               int priority = 0) {
    CHECK_NOTNULL(outs);
    if (outs->empty())
      outs->resize(vals.size());
    else
      CHECK_EQ(vals.size(), outs->size());

    SArray<Key> skeys(keys);
    SArray<Val> svals(vals);
    auto souts = new SArray<Val>(outs->data(), outs->size());
    SArray<int>* slens = lens ?
        new SArray<int>(lens->data(), lens->size()) : nullptr;
    int ts = ZPushPull(skeys, svals, souts, slens, cmd,
        [this, cb, souts, slens]() {
          delete souts;
          delete slens;
          if (cb) cb();
        }, priority);
    return ts;
  }

  /**
   * \brief Waits until a push or pull has been finished
   *
   * Sample usage:
   * \code
   *   int ts = w.Pull(keys, &vals);
   *   Wait(ts);
   *   // now vals is ready for use
   * \endcode
   *
   * \param timestamp the timestamp returned by the push or pull
   */
  void Wait(int timestamp) { obj_->WaitRequest(timestamp); }

  /**
   * \brief zero-copy Push
   *
   * This function is similar to \ref Push except that all data
   * will not be copied into system for better performance. It is the caller's
   * responsibility to keep the content to be not changed before actually
   * finished.
   */
  int ZPush(const SArray<Key>& keys,
            const SArray<Val>& vals,
            const SArray<int>& lens = {},
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
//    int ts = obj_->NewRequest(kServerGroup);
#ifdef LITTLE_GRAIN_MSG
      int ts = obj_->NewRequest(kServerGroup, keys.size());
#else
      int ts = obj_->NewRequest(kServerGroup);
#endif
    AddCallback(ts, cb);
    KVPairs<Val> kvs;
    kvs.keys = keys;
    kvs.vals = vals;
    kvs.lens = lens;
    kvs.priority = priority;
//    std::cout<<"Node-1 keys ZPush"<<DebugStr(keys.data(), keys.size())<<std::endl;
//    std::cout<<"node-1 send kvs(ZPush) "<<kvs.lens<<std::endl;
    Send(ts, true, false, cmd, kvs);
//    std::cout<<"Node-1 ts, cmd:(ZPush) "<<ts<<" "<<cmd<<std::endl;
//    std::cout<<"Node-1 ZPush!"<<std::endl;
    return ts;
  }

  /**
   * \brief zero-copy Pull
   *
   * This function is similar to \ref Pull except that all data
   * will not be copied into system for better performance. It is the caller's
   * responsibility to keep the content to be not changed before actually
   * finished.
   */
  int ZPull(const SArray<Key>& keys,
            SArray<Val>* vals,
            SArray<int>* lens = nullptr,
            int cmd = 0,
            const Callback& cb = nullptr,
            int priority = 0) {
//	std::cout<<"Node-1 keys ZPull "<<DebugStr(keys.data(), keys.size())<<std::endl;
    int ts = AddPullCB(keys, vals, lens, cmd, cb);
    KVPairs<Val> kvs;
    kvs.keys = keys;
    kvs.priority = priority;
    Send(ts, false, true, cmd, kvs);
    return ts;
  }

  /**
   * \brief zero-copy PushPull
   *
   * This function is similar to \ref PushPull except that all data
   * will not be copied into system for better performance. It is the caller's
   * responsibility to keep the content to be not changed before actually
   * finished.
   */
  int ZPushPull(const SArray<Key>& keys,
                const SArray<Val>& vals,
                SArray<Val>* outs,
                SArray<int>* lens = nullptr,
                int cmd = 0,
                const Callback& cb = nullptr,
                int priority = 0) {
//    std::cout<<"node-1 ZPushPull!"<<std::endl;
    int ts = AddPullCB(keys, outs, lens, cmd, cb);
    KVPairs<Val> kvs;
    kvs.keys = keys;
    kvs.vals = vals;
    kvs.priority = priority;
    if (lens)
      kvs.lens = *lens;
    Send(ts, true, true, cmd, kvs);
    return ts;
  }
  using SlicedKVs = std::vector<std::pair<bool, KVPairs<Val>>>;
  /**
   * \brief a slicer partitions a key-value list according to the key ranges
   * \param send the kv list for partitioning
   * \param ranges the key ranges, ranges[i] is the key range of server i
   * \param sliced the sliced lists. slices[i] should only contains keys in
   * ranges[i] and the according values
   */
  using Slicer = std::function<void(
      const KVPairs<Val>& send, const std::vector<Range>& ranges,
      SlicedKVs* sliced)>;

  /**
   * \brief set a user-defined slicer
   */
  void set_slicer(const Slicer& slicer) {
    CHECK(slicer); slicer_ = slicer;
  }

 private:
  /**
   * \brief internal pull, C/D can be either SArray or std::vector
   */
  template <typename C, typename D>
  int AddPullCB(const SArray<Key>& keys, C* vals, D* lens,
            int cmd, const Callback& cb);
  /**
   * \brief add a callback for a request. threadsafe.
   * @param cb callback
   * @param timestamp the timestamp of the request
   */
  void AddCallback(int timestamp, const Callback& cb) {
    if (!cb) return;
    std::lock_guard<std::mutex> lk(mu_);
    callbacks_[timestamp] = cb;
  }

  /**
   * \brief run and delete the callback
   * \param timestamp the timestamp of the callback
   */
  void RunCallback(int timestamp);
  /**
   * \brief send the kv list to all servers
   * @param timestamp the timestamp of the request
   * @param push whether or not it is a push request
   * @param push whether or not it is a pull request
   * @param cmd command
   */
  void Send(int timestamp, bool push, bool pull, int cmd, const KVPairs<Val>& kvs);
  /** \brief internal receive handle */
  void Process(const Message& msg);
  /** \brief default kv slicer */
  void DefaultSlicer(const KVPairs<Val>& send,
                     const std::vector<Range>& ranges,
                     SlicedKVs* sliced);

  /** \brief data buffer for received kvs for each timestamp */
  std::unordered_map<int, std::vector<KVPairs<Val>>> recv_kvs_;
  /** \brief callbacks for each timestamp */
#ifdef EVAL_CONTRIBUTE_CON
        void Open_loss_file();
        void init_dgt();
#ifdef ADAPTIVE_K
        float adaptive_k();
        float throughput_rt=0.0;
        float delta_tp = 0.0;
        float first_loss = 0.0;
        float rt_loss = 0.0;
#endif
        float dmlc_k = 1.0;
        float dmlc_k_init = 1.0;
        float dmlc_k_min = 0.0;
        int   adaptive_k_flag = 0;
        int udp_channel_num = 0;
        int enable_send_drop = 0;
        std::vector<int> index_vec;
        void Update_loss_delta();
        float Evaluate_msg_contri(int key, Message& msg);
        float mse(int key, int block_size, SArray<Val>& vals);
        int Get_channel(int index, int max_index, int C, float k);
        int Aproximate_channel_estimate(Message& msg,int C);
        void Update_contri_max(int key, int seq, int seq_end,float contri);
        int64_t push_op_num = 0;
        int enable_block = 0;
        int block_size = 0;
        int test_block_size = 0;
        int enable_dgt = 0;
        int clear_zero = 0;
        std::unordered_map<int, float> pre_max_N;
        float max_N = 0.0;
        float contri_alpha = 0.3;
        int set_random = 0;
        int dgt_info = 0;
        float p_N = 0.0;
        float max_contri = 0.0;
        std::unordered_map<int, float> contri_max;
        std::unordered_map<int, float> pre_contri_max;
        FILE *fp;

        std::unordered_map<int, float> p_loss;
        std::unordered_map<int, std::unordered_map<int, float>> contri;
        std::vector<Message> msg_vector;
        std::vector<Message_RU> rank_vector;
        float pre_loss = 0;
        float delta_l = 0.0;
#endif
  std::unordered_map<int, Callback> callbacks_;
  /** \brief lock */
  std::mutex mu_;
  /** \brief kv list slicer */
  Slicer slicer_;
#ifdef DOUBLE_CHANNEL
        bool is_first_push_op = false;
  std::unordered_set<int> is_first_push_;
#endif
};

/** \brief meta information about a kv request */
struct KVMeta {
  /** \brief the int cmd */
  int cmd;
  /** \brief whether or not this is a push request */
  bool push;
  /** \brief whether or not this is a pull request */
  bool pull;
  /** \brief sender's node id */
  int sender;
  /** \brief the associated timestamp */
  int timestamp;
#ifdef LITTLE_GRAIN_MSG
        int tracker_num;
#endif
#ifdef UDP_CHANNEL
        int first_key;
        int keys_len;
        int vals_len;
        int lens_len;
        //std::vector<int> data_len;
        int key_begin;
        int key_end;
        int channel;
#endif
  /** \brief the customer id of worker */
  int customer_id;
};

/**
 * \brief A server node for maintaining key-value pairs
 */
template <typename Val>
class KVServer : public SimpleApp {
 public:
  /**
   * \brief constructor
   * \param app_id the app id, should match with \ref KVWorker's id
   */
  explicit KVServer(int app_id) : SimpleApp() {
    using namespace std::placeholders;
    obj_ = new Customer(app_id, app_id, std::bind(&KVServer<Val>::Process, this, _1));
  }

  /** \brief deconstructor */
  virtual ~KVServer() { delete obj_; obj_ = nullptr; }

  /**
   * \brief the handle to process a push/pull request from a worker
   * \param req_meta meta-info of this request
   * \param req_data kv pairs of this request
   * \param server this pointer
   */
  using ReqHandle = std::function<void(const KVMeta& req_meta,
                                       const KVPairs<Val>& req_data,
                                       KVServer* server)>;
  void set_request_handle(const ReqHandle& request_handle) {
    CHECK(request_handle) << "invalid request handle";
    request_handle_ = request_handle;
  }

  /**
   * \brief response to the push/pull request
   * \param req the meta-info of the request
   * \param res the kv pairs that will send back to the worker
   */
  void Response(const KVMeta& req, const KVPairs<Val>& res = KVPairs<Val>());

 private:
  /** \brief internal receive handle */
  void Process(const Message& msg);
  /** \brief request handle */
  ReqHandle request_handle_;
    std::unordered_map<int,int> tag_map;
};


/**
 * \brief an example handle adding pushed kv into store
 */
template <typename Val>
struct KVServerDefaultHandle {
  void operator()(
      const KVMeta& req_meta, const KVPairs<Val>& req_data, KVServer<Val>* server) {
    size_t n = req_data.keys.size();
    KVPairs<Val> res;
    if (!req_meta.pull) {
      CHECK_EQ(n, req_data.vals.size());
    } else {
      res.keys = req_data.keys; res.vals.resize(n);
    }
    for (size_t i = 0; i < n; ++i) {
      Key key = req_data.keys[i];
      if (req_meta.push) {
        store[key] += req_data.vals[i];
      }
      if (req_meta.pull) {
        res.vals[i] = store[key];
      }
    }
    server->Response(req_meta, res);
  }
  std::unordered_map<Key, Val> store;
};


///////////////////////////////////////////////////////////////////////////////

template <typename Val>
void KVServer<Val>::Process(const Message& msg) {
  if (msg.meta.simple_app) {
    SimpleApp::Process(msg); return;
  }
  KVMeta meta;
  meta.cmd       = msg.meta.head;
  meta.push      = msg.meta.push;
  meta.pull      = msg.meta.pull;
  meta.sender    = msg.meta.sender;
  meta.timestamp = msg.meta.timestamp;
  meta.customer_id = msg.meta.customer_id;
  KVPairs<Val> data;
  int n = msg.data.size();
  if (n) {
    CHECK_GE(n, 2);
    data.keys = msg.data[0];
    data.vals = msg.data[1];
    if (n > 2) {
      CHECK_EQ(n, 3);
      data.lens = msg.data[2];
      CHECK_EQ(data.lens.size(), data.keys.size());
    }
  }
  CHECK(request_handle_);
  request_handle_(meta, data, this);
}

template <typename Val>
void KVServer<Val>::Response(const KVMeta& req, const KVPairs<Val>& res) {
  Message msg;
  msg.meta.app_id = obj_->app_id();
  msg.meta.customer_id = req.customer_id;
  msg.meta.request     = false;
  msg.meta.push        = req.push;
  msg.meta.pull        = req.pull;
  msg.meta.head        = req.cmd;
  msg.meta.timestamp   = req.timestamp;
  msg.meta.recver      = req.sender;
  if (res.keys.size()) {
    msg.AddData(res.keys);
    msg.meta.keys_len = msg.data.back().size();
    msg.AddData(res.vals);
    msg.meta.vals_len = msg.data.back().size();
    if (res.lens.size()) {
      msg.AddData(res.lens);
      msg.meta.lens_len = msg.data.back().size();
    }
  }
  Postoffice::Get()->van()->Send(msg);
}

template <typename Val>
void KVWorker<Val>::DefaultSlicer(
    const KVPairs<Val>& send, const std::vector<Range>& ranges,
    typename KVWorker<Val>::SlicedKVs* sliced) {
  sliced->resize(ranges.size());

  // find the positions in msg.key
  size_t n = ranges.size();
  std::vector<size_t> pos(n+1);
  const Key* begin = send.keys.begin();
  const Key* end = send.keys.end();
  for (size_t i = 0; i < n; ++i) {
    if (i == 0) {
      pos[0] = std::lower_bound(begin, end, ranges[0].begin()) - begin;
      begin += pos[0];
    } else {
      CHECK_EQ(ranges[i-1].end(), ranges[i].begin());
    }
    size_t len = std::lower_bound(begin, end, ranges[i].end()) - begin;
    begin += len;
    pos[i+1] = pos[i] + len;

    // don't send it to servers for empty kv
    sliced->at(i).first = (len != 0);
  }
  CHECK_EQ(pos[n], send.keys.size());
  if (send.keys.empty()) return;

  // the length of value
  size_t k = 0, val_begin = 0, val_end = 0;
  if (send.lens.empty()) {
    k = send.vals.size() / send.keys.size();
    CHECK_EQ(k * send.keys.size(), send.vals.size());
  } else {
    CHECK_EQ(send.keys.size(), send.lens.size());
  }

  // slice
  for (size_t i = 0; i < n; ++i) {
    if (pos[i+1] == pos[i]) {
      sliced->at(i).first = false;
      continue;
    }
    sliced->at(i).first = true;
    auto& kv = sliced->at(i).second;
    kv.keys = send.keys.segment(pos[i], pos[i+1]);
    if (send.lens.size()) {
      kv.lens = send.lens.segment(pos[i], pos[i+1]);
      for (int l : kv.lens) val_end += l;
      kv.vals = send.vals.segment(val_begin, val_end);
      val_begin = val_end;
    } else {
      kv.vals = send.vals.segment(pos[i]*k, pos[i+1]*k);
    }
  }
}
#ifdef EVAL_CONTRIBUTE_CON
    template <typename Val>
    void KVWorker<Val>::Open_loss_file() {
        std::string file_str = "/tmp/loss"+ std::to_string(Postoffice::Get()->van()->my_node().id)+ ".csv";
        std::cout << "file_str = " << file_str << std::endl;
        fp = fopen(file_str.c_str(),"w+");
        if(!fp){
            std::cout << "failed to open csv file" << std::endl;
        }else{
            std::cout << "open loss csv success" << std::endl;
        }
    }
    template <typename Val>
    void KVWorker<Val>::Update_loss_delta() {
        char line[10];
        float cur_loss = 0.0;
        if(fgets(line, 10, fp) != NULL){
            cur_loss = atof(line);
            //std::cout << "loss = " << cur_loss << std::endl;
            fseek(fp,0,0);
        }
        //std::cout << "cur_loss = " << cur_loss << std::endl;
        if(pre_loss != 0){
            delta_l = pre_loss - cur_loss;
        }else{
            delta_l = 1;
        }
        //std::cout << "delta_l = " << delta_l << std::endl;
        pre_loss = cur_loss;
#ifdef ADAPTIVE_K
        rt_loss = cur_loss;
        if(first_loss ==0.0){ first_loss = cur_loss;}
#endif
    }
    template <typename Val>
    float KVWorker<Val>::mse(int key, int block_size, SArray<Val>& vals) {
        int total_bytes = vals.size();
        int remain_bytes = total_bytes;
        int val_bytes = 0;
        int seq = 0;
        int seq_num = 0;
        if(total_bytes % block_size == 0){
            seq_num = total_bytes/block_size;
        }else{
            seq_num = total_bytes/block_size + 1;
        }
        float mt = 0;
        int lt = 0;

        while(remain_bytes != 0){
            int l = std::min(remain_bytes,block_size);
            SArray<Val> tmp_val = vals.segment(val_bytes, val_bytes+l);
            val_bytes += l;
            float mse_t = 0;
            float *pd = (float*)tmp_val.data();
            int nlen = tmp_val.size() / sizeof(float);
            float N = 0.0;
            for(int i = 0; i < nlen; i++){
                N += fabs(*(pd+i));
            }

            for(int i = 0; i < nlen; i++){
                mse_t += pow(fabs(fabs(*(pd+i)) - N/nlen),2);
            }
            mt += mse_t;
            lt += nlen;
            remain_bytes -= l;
        }
        std::cout << key << "," << mt/lt << std::endl;
    }
    template <typename Val>
    void KVWorker<Val>::Update_contri_max(int key, int seq, int seq_end,float contri) {
        if(contri_max.find(key)==contri_max.end() || seq == 0) contri_max[key] = 0.0;
        if(pre_contri_max.find(key)==pre_contri_max.end()) pre_contri_max[key] = 0.0;
        if(contri > contri_max[key]) contri_max[key] = contri;
        //std::cout << "contri_max[" << key << "]" << contri_max[key] << std::endl;
        if(seq == seq_end)  {

            pre_contri_max[key] = contri_max[key];
        }

    }
    template <typename Val>
    float KVWorker<Val>::Evaluate_msg_contri(int key, Message& msg) {
        /*calculate p_N of a msg*/
        float *pd = (float*)msg.data[1].data();
        int nlen = msg.data[1].size() / sizeof(float);
        float N = 0.0;
        for(int i = 0; i < nlen; i++){
            N += fabs(*(pd+i));
        }

        /*calculate contri of a msg*/
        auto itt = contri.find(key);
        if(itt == contri.end()) contri[key][msg.meta.seq] = 0.0;
        auto it = contri[key].find(msg.meta.seq);
        if(it == contri[key].end()) contri[key][msg.meta.seq] = 0.0;

        contri[key][msg.meta.seq] = contri_alpha * contri[key][msg.meta.seq] + (1-contri_alpha)*(N/nlen);
        //if(key == 0 && msg.meta.seq == 0)
        //std::cout << "contri[" << key << "][" << msg.meta.seq << "]" << contri[key][msg.meta.seq] << "," << N << "/" << nlen << " = " << N/nlen << std::endl;
        Update_contri_max(key,msg.meta.seq,msg.meta.seq_end,contri[key][msg.meta.seq]);//////
        return contri[key][msg.meta.seq];
    }
    template <typename Val>
    int KVWorker<Val>::Aproximate_channel_estimate(Message& msg,int C) {
        float p;
        if(contri_max[msg.meta.first_key] != 0){
            //p = msg.contri/pre_contri_max[msg.meta.first_key];
            p = msg.contri/contri_max[msg.meta.first_key];
            //std::cout << "key = " << msg.meta.first_key << "seq = " << msg.meta.seq << ",p=" << p << std::endl;
        }else{
            p = 1;
        }

        if(p >= 1) return 0;
        if(p == 0) return 9;
        int channel_num = C+1;
        int channel = 0;
        int i = 0;
        srand((unsigned)time(NULL));
        for(; i < C; i++){
            float min =  i*1.0/C;
            float max =  (i+1)*1.0/C;
            if(p >= min && p < max){
                float lp = (max-p)/(max-min);
                if((rand()%100+1)/100.0 <= lp){
                    channel = i;
                }else{
                    channel = i+1;
                }
            }
            break;

        }
        return C-channel;
    }
    template <typename Val>
    int KVWorker<Val>::Get_channel(int index, int max_index, int C, float k) {

        int min_index = std::round(k*(max_index+1));
        if(index < min_index){
            return 0;
        }

        for(int i = 0; i < C; ++i){
            if(max_index - min_index > 0){
                if(index >= min_index + (float)i * (max_index-min_index)/C && index < min_index + (float)(i+1) * (max_index-min_index)/C){
                    //std::cout << "index = " << index <<" min_index = " << min_index << "max_index = " << max_index <<"i = " << i << std::endl;
                    return i+1;
                }
            }else{
                return i+1;
            }

        }
        srand((unsigned)time(NULL));
        //int rn = rand();
        return rand()%C+1;
        //return rn%7 + 1;
    }
#ifdef ADAPTIVE_K
    template <typename Val>
    float KVWorker<Val>::adaptive_k(){

        float k;
        if(dmlc_k_init*(rt_loss/first_loss) > dmlc_k_min){
            k = dmlc_k_init*(rt_loss/first_loss);
        }else{
            k = dmlc_k_min;
        }
        //float k = std::f{dmlc_k_init*(rt_loss/first_loss),0.2};
        return k;
    }
#endif
    template <typename Val>
    void KVWorker<Val>::init_dgt(){
        Open_loss_file();
        dmlc_k_init = atof(CHECK_NOTNULL(Environment::Get()->find("DMLC_K")));
        dmlc_k_min = atof(CHECK_NOTNULL(Environment::Get()->find("DMLC_K_MIN")));
        adaptive_k_flag = atoi(CHECK_NOTNULL(Environment::Get()->find("ADAPTIVE_K_FLAG")));
        udp_channel_num = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_UDP_CHANNEL_NUM")));
        //enable_send_drop = atoi(CHECK_NOTNULL(Environment::Get()->find("ENABLE_SEND_DROP")));
        
        return;
    }
#endif
#ifdef PARAM_
    template <typename Val>
std::vector<Message> KVWorker<Val>::split_msg(Message& msg){
    int key = msg.meta.first_key;
    int len = *(int *)msg.data[2].data();
    float p = (float*)msg.data[1].data();
    auto it  = contri_vec.find(key);
    if(it == contri_vec.end()){
        std::vector<int> l(len,0);
        contri_vec[key] = l;
    }
    for(int i = 0; i < len; ++i){
        contri_vec[key] += contri_alpha*contri_vec[key] + (1-contri_alpha)*fabs(*(p+i));
    }

}
#endif
template <typename Val>
void KVWorker<Val>::Send(int timestamp, bool push, bool pull, int cmd, const KVPairs<Val>& kvs) {
  // slice the message
//	std::cout<<"node-1 start to send!!"<<std::endl;
  SlicedKVs sliced;
  slicer_(kvs, Postoffice::Get()->GetServerKeyRanges(), &sliced);

  // need to add response first, since it will not always trigger the callback
  int skipped = 0;
  for (size_t i = 0; i < sliced.size(); ++i) {
    if (!sliced[i].first) ++skipped;
  }
  obj_->AddResponse(timestamp, skipped);
  if ((size_t)skipped == sliced.size()) {
    RunCallback(timestamp);
  }
//std::cout<<"node-1 start to check 1"<<std::endl;
  for (size_t i = 0; i < sliced.size(); ++i) {
    const auto& s = sliced[i];
    if (!s.first) continue;
   /* Message msg;
    msg.meta.app_id = obj_->app_id();
    msg.meta.customer_id = obj_->customer_id();
    msg.meta.request     = true;
    msg.meta.push        = push;
    msg.meta.pull        = pull;
    msg.meta.head        = cmd;
    msg.meta.timestamp   = timestamp;
    msg.meta.recver      = Postoffice::Get()->ServerRankToID(i);
    msg.meta.priority    = kvs.priority;*/
    const auto& kvs = s.second;
      if(push){
	    //  std::cout<<"node-1 start to check-3!!"<<std::endl;
          if(kvs.keys[0] == 0){
//		  std::cout<<"node-1 kvs.keys[0] "<<kvs.keys[0]<<std::endl;
              push_op_num++;
              if(push_op_num > 1){
//		      std::cout<<"node-1 push_op_num "<<push_op_num<<std::endl;
                  Update_loss_delta();
                  if(adaptive_k_flag){
//			  std::cout<<"node-1 start to check-6!!"<<std::endl;
                      dmlc_k = adaptive_k();

                  }else{
                      dmlc_k = dmlc_k_init;
                  }
                 if(dgt_info)
                     //std::cout << "dmlc_k = " << dmlc_k << std::endl;
std::cout << std::endl;
              }else{
		     // std::cout<<"node-1 init_dgt()-1;"<<std::endl;
                  init_dgt();
//		  std::cout<<"node-1 init_dgt();"<<std::endl;
              }

          }
//	  std::cout<<"node-1 first  push"<<std::endl;
          if(push_op_num == 1){     //first push
              Message msg;
              msg.meta.app_id = obj_->app_id();
              msg.meta.customer_id = obj_->customer_id();
              msg.meta.request     = true;
              msg.meta.push        = push;
	      msg.meta.pull        = pull;
              msg.meta.head        = cmd;
              msg.meta.timestamp   = timestamp;
              msg.meta.recver      = Postoffice::Get()->ServerRankToID(i);
              msg.meta.msg_type = 1;
              msg.meta.first_key = kvs.keys[0];
              msg.meta.seq = 0;
              msg.meta.seq_begin = 0;
              msg.meta.seq_end = 0;
              msg.meta.val_bytes = 0;
              msg.meta.total_bytes = kvs.vals.size();
              msg.meta.push_op_num = push_op_num;
              if (kvs.keys.size()) {
//		      std::cout<<"node-1 first push kv.keys.size() "<<kvs.keys.size()<<std::endl;
                  msg.AddData(kvs.keys);
                  msg.meta.keys_len = msg.data.back().size();
                  msg.AddData(kvs.vals);
                  msg.meta.vals_len = msg.data.back().size();
                  if (kvs.lens.size()) {
                      msg.AddData(kvs.lens);
                      msg.meta.lens_len = msg.data.back().size();
                  }
              }
              Postoffice::Get()->van()->Send(msg);
	     // std::cout<<"node-1 check point-1"<<std::endl;
          }else{
//	      std::cout<<"node-1 check point-10"<<std::endl;
              int total_bytes = kvs.vals.size();
              int remain_bytes = total_bytes;
              int val_bytes = 0;
              int seq = 0;
              int seq_num = 0;

              if(enable_block == 0)
                  block_size = total_bytes;
              if(total_bytes % block_size == 0){
                  seq_num = total_bytes/block_size;
              }else{
                  seq_num = total_bytes/block_size + 1;
              }
              std::vector<int> count(udp_channel_num+1,0);
              int count_zero = 0;
              while(remain_bytes != 0){
                  Message msg;
                  msg.meta.app_id = obj_->app_id();
                  msg.meta.customer_id = obj_->customer_id();
                  msg.meta.request     = true;
                  msg.meta.push        = push;
		  msg.meta.pull        = pull;
                  msg.meta.head        = cmd;
                  msg.meta.timestamp   = timestamp;
                  msg.meta.recver      = Postoffice::Get()->ServerRankToID(i);
                  msg.meta.msg_type = 2;
                  msg.meta.push_op_num = push_op_num;
                  msg.meta.total_bytes = total_bytes;
                  
                  int l = std::min(remain_bytes,block_size);
                  SArray<Val> tmp_val = kvs.vals.segment(val_bytes, val_bytes+l);
                  ////////////////
                  //mse(kvs.keys[0],test_block_size,tmp_val);
                  //////////////////
                  msg.meta.val_bytes = val_bytes;
                  val_bytes += l;
                  msg.meta.first_key = kvs.keys[0];
                  msg.meta.seq = seq;
                  msg.meta.seq_begin = 0;
                  msg.meta.seq_end = seq_num-1;
                  if (kvs.keys.size()) {
                      msg.AddData(kvs.keys);
                      msg.meta.keys_len = msg.data.back().size();
                      msg.AddData(tmp_val);
                      msg.meta.vals_len = msg.data.back().size();
                      if (kvs.lens.size()) {
                          msg.AddData(kvs.lens);
                          msg.meta.lens_len = msg.data.back().size();
                      }
                  }
                  msg.contri = Evaluate_msg_contri((int)kvs.keys[0], msg);
                  if(clear_zero){
                      if(msg.contri != 0 || msg.meta.seq == msg.meta.seq_end) msg_vector.push_back(msg);
                  }
                  else
                      msg_vector.push_back(msg);
                  remain_bytes -= l;
                  seq++;
                  msg.data.clear();


              }
              if(set_random){
                  auto engine = std::default_random_engine{};
                  std::shuffle(std::begin(msg_vector), std::end(msg_vector)-1, engine);
              }else{
                  std::sort(msg_vector.begin(),msg_vector.end()-1,[](const Message& msg1, const Message& msg2){
                      return msg1.contri > msg2.contri;
                  });
              }
              for(size_t j = 0; j < msg_vector.size(); ++j){
                  msg_vector[j].meta.channel = Get_channel(j, msg_vector.size()-1, udp_channel_num, dmlc_k);
                  if(msg_vector[j].meta.seq == msg_vector[j].meta.seq_end) {
                      msg_vector[j].meta.channel=0;
                  }
                  if(enable_dgt){
                      Postoffice::Get()->van()->Classifier(msg_vector[j],msg_vector[j].meta.channel,0);
                  }else{
                      Postoffice::Get()->van()->Send(msg_vector[j],0,0);
                  }

              }
              msg_vector.clear();
          }
      }else{                               //pull
//          std::cout<<"node-1 checkpoint-2"<<std::endl;
	  Message msg;
          msg.meta.app_id = obj_->app_id();
          msg.meta.customer_id = obj_->customer_id();
          msg.meta.request     = true;
          msg.meta.push        = push;
	  msg.meta.pull        = pull;
          msg.meta.head        = cmd;
          msg.meta.timestamp   = timestamp;
          msg.meta.recver      = Postoffice::Get()->ServerRankToID(i);
          msg.meta.msg_type = 3;
          msg.meta.first_key = kvs.keys[0];
          msg.meta.seq = 0;
          msg.meta.seq_begin = 0;
          msg.meta.seq_end = 0;
          msg.meta.val_bytes = 0;
          msg.meta.total_bytes = kvs.vals.size();
          if (kvs.keys.size()) {
              msg.AddData(kvs.keys);
              msg.meta.keys_len = msg.data.back().size();
              msg.AddData(kvs.vals);
              msg.meta.vals_len = msg.data.back().size();
              if (kvs.lens.size()) {
                  msg.AddData(kvs.lens);
                  msg.meta.lens_len = msg.data.back().size();
              }
          }
          Postoffice::Get()->van()->Send(msg);
      }


  }
//  std::cout<<"node-1 end send operation!"<<std::endl;
}


template <typename Val>
void KVWorker<Val>::Process(const Message& msg) {
  if (msg.meta.simple_app) {
    SimpleApp::Process(msg); return;
  }
  // store the data for pulling
  int ts = msg.meta.timestamp;

  if (msg.meta.pull) {
    CHECK_GE(msg.data.size(), (size_t)2);
    KVPairs<Val> kvs;
    kvs.keys = msg.data[0];
//    std::cout<<"Node-1 Process kvs.keys "<<DebugStr(kvs.keys.data(),kvs.keys.size())<<std::endl;
    kvs.vals = msg.data[1];
    if (msg.data.size() > (size_t)2) {
      kvs.lens = msg.data[2];
    }
    mu_.lock();
//    std::cout<<"node-1 worker process!"<<std::endl;
    recv_kvs_[ts].push_back(kvs);
    mu_.unlock();
  }
#ifdef LITTLE_GRAIN_MSG_OFF
    if(msg.meta.push){
		if (msg.meta.first_key == msg.meta.key_end)  {
            RunCallback(ts);
		}
	}else{
		if (obj_->NumResponse(ts) == msg.meta.tracker_num-1)  {
		RunCallback(ts);
		}
	}
	//
#else

  // finished, run callbacks
  if (obj_->NumResponse(ts) == Postoffice::Get()->num_servers() - 1)  {
    RunCallback(ts);
  }
#endif
}
template <typename Val>
void KVWorker<Val>::RunCallback(int timestamp) {
  mu_.lock();
  auto it = callbacks_.find(timestamp);
  if (it != callbacks_.end()) {
    mu_.unlock();

    CHECK(it->second);
    it->second();

    mu_.lock();
    callbacks_.erase(it);
  }
  mu_.unlock();
}

template <typename Val>
template <typename C, typename D>
int KVWorker<Val>::AddPullCB(
    const SArray<Key>& keys, C* vals, D* lens, int cmd,
    const Callback& cb) {
#ifdef LITTLE_GRAIN_MSG
  int ts = obj_->NewRequest(kServerGroup);
 // std::cout<<"ts LITTLE_GRAIN_MSG "<<ts<<std::endl;
#else
    int ts = obj_->NewRequest(kServerGroup);
   //   std::cout<<"ts LITTLE_GRAIN_MSG "<<ts<<std::endl;
#endif
  AddCallback(ts, [this, ts, keys, vals, lens, cb]() mutable {
      mu_.lock();
      auto& kvs = recv_kvs_[ts];
      mu_.unlock();

      // do check
      size_t total_key = 0, total_val = 0;
      for (auto& s : kvs) {
        Range range = FindRange(keys, s.keys.front(), s.keys.back()+1);
        CHECK_EQ(range.size(), s.keys.size())
            << "unmatched keys size from one server"<< keys <<"("<<range.begin() << "," << range.end() << ")"<<s.keys;
        if (lens) CHECK_EQ(s.lens.size(), s.keys.size());
//      	std::cout<<"node-1 s.keys.size()"<<std::endl;
      	total_key += s.keys.size();
        total_val += s.vals.size();
      }
//            std::cout<<"Node-1 total_key: "<<total_key<<", keys.size(): "<<keys.size()<<std::endl;
      CHECK_EQ(total_key, keys.size()) << "lost some servers?";
//      std::cout<<"Node-1 wrong! "<<std::endl;

      // fill vals and lens
      std::sort(kvs.begin(), kvs.end(), [](
          const KVPairs<Val>& a, const KVPairs<Val>& b) {
                  return a.keys.front() < b.keys.front();
        });
      CHECK_NOTNULL(vals);
      if (vals->empty()) {
        vals->resize(total_val);
      } else {
        CHECK_EQ(vals->size(), total_val);
      }
      Val* p_vals = vals->data();
      int *p_lens = nullptr;
      if (lens) {
        if (lens->empty()) {
          lens->resize(keys.size());
        } else {
          CHECK_EQ(lens->size(), keys.size());
        }
        p_lens = lens->data();
      }
      for (const auto& s : kvs) {
        memcpy(p_vals, s.vals.data(), s.vals.size() * sizeof(Val));
        p_vals += s.vals.size();
        if (p_lens) {
          memcpy(p_lens, s.lens.data(), s.lens.size() * sizeof(int));
          p_lens += s.lens.size();
        }
      }

      mu_.lock();
      recv_kvs_.erase(ts);
      mu_.unlock();
      if (cb) cb();
    });
//  std::cout<<"node-1 pull ts "<<ts<<std::endl;
  return ts;
}

}  // namespace ps
#endif  // PS_KV_APP_H_
