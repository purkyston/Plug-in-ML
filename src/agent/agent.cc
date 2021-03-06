// Copyright 2018 The RPSCC Authors. All Rights Reserved.
// Author : Chenbin Zhang (zcbin@pku.edu.cn)

#include <stdio.h>

#include <algorithm>
#include <set>

#include "src/agent/agent.h"
#include "src/communication/zmq_communicator.h"
#include "src/message/message.pb.h"
#include "src/util/logging.h"
#include "src/util/network_util.h"
#include "agent.h"

using std::cout;
using std::endl;

namespace rpscc {

// Macro for getting the Agent's IP address
DEFINE_string(net_interface, "",
              "Name of the net interface used by the node.");

// This is a sorter for key list and value list sorted in the agent. During the
// sorting, keys and values will keep their relative positions.
void Agent::SortKeyValue(int32* keys, float32* values, int32 size) {
  std::map<int32, int32> key_map;
  std::vector<float32> tmp_values(values, values + size);

  for (int32 i = 0; i < size; i++) key_map.insert(std::make_pair(keys[i], i));
  size = 0;
  for (auto item : key_map) {
    keys[size] = item.first;
    values[size++] = tmp_values[item.second];
  }
  tmp_values.clear();
  key_map.clear();
}

// To Initialize the agent.
bool Agent::Initialize(std::string para_fifo_name, 
                       std::string grad_fifo_name,
                       std::string para_memory_name,
                       std::string grad_memory_name,
                       std::string master_addr,
                       const int16& listen_port) {
  // 1.Initialize sender

  cout << "1.Initialize sender" << endl;

  sender_.reset(new ZmqCommunicator());
  if (sender_.get() == NULL) {
    printf("Initialize sender failed.");
    return false;
  }
  sender_->Initialize(64/* ring_size */, true, 1024/* listen_port */);

  // 2.Initialize receiver

  cout << "2.Initialize receiver" << endl;

  receiver_.reset(new ZmqCommunicator());
  if (receiver_.get() == NULL) {
    printf("Initialize receiver failed.");
    return false;
  }
  listen_port_ = listen_port;
  receiver_->Initialize(64/* ring_size */, false, listen_port_);

  // 3 Exchange messages with master
  // 3_1.Send this agent's local_ip_ to master, and receive config information
  std::string ip = "";
  GetIP(FLAGS_net_interface, &ip);
  if (ip == "") {
    LOG(ERROR) << "Cannot find IP from the interface provided.";
    return false;
  }

  cout << "3_1 Agent's <ip>:<port> is " << ip << " : " << listen_port_
       << endl;

  local_ip_ = ip;
  Message msg_send;
  Message msg_recv;
  // Send register message to master, and receive config_msg.
  Message_RegisterMessage* reg_msg_ptr = new Message_RegisterMessage();
  Message_ConfigMessage config_msg;
  std::string reg_str;
  std::string config_str;

  reg_msg_ptr->set_ip(local_ip_);
  reg_msg_ptr->set_port(listen_port_);
  reg_msg_ptr->set_is_server(false);
  msg_send.set_message_type(Message_MessageType_register_);
  msg_send.set_recv_id(0);
  msg_send.set_send_id(-1);
  
  msg_send.set_allocated_register_msg(reg_msg_ptr);
  msg_send.SerializeToString(&reg_str);
  cout << "3_1 Sending register message to master" << endl;
  sender_->AddIdAddr(0, master_addr);
  if (sender_->Send(0, reg_str) == -1) {
    LOG(INFO) << "Cannot send register message to master" << endl;
    LOG(ERROR) << "Cannot send register message to master";
    return false;
  }
  cout << "3_2 Receiving config string from master" << endl;
  if (receiver_->Receive(&config_str) == -1) {
    LOG(ERROR) << "Error in receiving message from master";
    return false;
  }

  cout << "config_str.size() = " << config_str.size() << endl;
  msg_recv.ParseFromString(config_str);
  config_msg = msg_recv.config_msg();
  // 3_2.Initialization of agent fields 
  local_id_ = msg_recv.recv_id();
  agent_num_ = config_msg.worker_num();
  server_num_ = config_msg.server_num();
  key_range_  = config_msg.key_range();

  cout << "3_2 Initialization " << "local_id = " << local_id_
       << " agent_num_ = " << agent_num_ << " server_num_ = "
       << server_num_ << " key_range_ = " << key_range_ << endl;

  // 3_3.Add <server_id, server_addr> pairs to the sender_'s <Id, Addr> map.
  server_ids_.clear();
  for (int32 i = 0; i < server_num_; i++) {
    int32 server_i_id = config_msg.server_id(i);
    server_ids_.push_back(server_i_id);
    cout << "server_id = " << server_i_id;
    std::string server_i_ip_port = config_msg.node_ip_port(server_i_id);

    cout << " ip_port = " << server_i_ip_port
         << endl;
    
    sender_->AddIdAddr(server_i_id, server_i_ip_port);
  }

  cout << "3_3.Add <server_id, server_addr> pairs to the sender_'s"
       << " <Id, Addr> map." << endl;

  // 3_4.Initialize the partition_.
  cout << "3_4.Initialize the partition_." << endl;

  int32* part_vec = new int32[server_num_];
  config_msg.mutable_partition()->ExtractSubrange(0, server_num_,
                                                  part_vec);
  partition_.Initialize(key_range_, server_num_, part_vec);
  delete[] part_vec;

   // 3_5.Initialize master ids.
  for (uint32 i = 0; i < config_msg.master_id_size(); ++i) {
    master_ids_.push_back(config_msg.master_id(i));
  }

  // 4.Initialize the fifo and shared memory
  cout << "4.Initialize the fifo and shared memory" << endl;
  para_fifo_name_ = para_fifo_name;
  grad_fifo_name_ = grad_fifo_name;
  para_memory_name_ = para_memory_name;
  grad_memory_name_ = grad_memory_name;

  int32 fd = shm_open(para_memory_name_.c_str(), O_RDWR | O_CREAT, FILE_MODE);
  ftruncate(fd, sizeof(struct shmstruct));
  close(fd);
  fd = shm_open(grad_memory_name_.c_str(), O_RDWR | O_CREAT, FILE_MODE);
  ftruncate(fd, sizeof(struct shmstruct));
  close(fd);

  // Agent is reader for parameters and writer for gradients
  mkfifo(para_fifo_name_.c_str(), 0777);
  mkfifo(grad_fifo_name_.c_str(), 0777);
  para_fifo_.Initialize(para_fifo_name_, false);
  grad_fifo_.Initialize(grad_fifo_name_, true);
  para_memory_.Initialize(para_memory_name_.c_str());
  grad_memory_.Initialize(grad_memory_name_.c_str());

  // 5.Set the epoch_num_ to 0
  epoch_num_ = 0;

  // 6.Set the reconfig_msg_ to NULL
  reconfig_msg_ = NULL;

  LOG(INFO) << "Agent's initialization is done" << endl;

  return true;
}

void Agent::Finalize() {
  sender_->Finalize();
  receiver_->Finalize();
  partition_.Finalize();
  cout << "Agent's Finalization is done" << endl;
}

bool Agent::Start() {
  // Start a thread to support the feature of heartbeat
  cout << "Start a thread to support the feature of heartbeat" << endl;
  pthread_create(&heartbeat_, NULL, HeartBeat, reinterpret_cast<void*>(this));

  para_fifo_.Open();
  grad_fifo_.Open();
  if (!AgentWork()) {
    LOG(ERROR) << "Agent work failed.";
    return false;
  }
  return true;
}

bool Agent::AgentWork() {
  cout << "Agent: Start AgentWork" << endl;
  int32 signal_type;
  while (true) {
    // Wait for worker's signal
    cout << "Agent: Wait for worker's signal" << endl;
    signal_type = grad_fifo_.Wait();

    // In the beginning of every loop, the agent's main thread will check the reconfig_flag_
    // Lock the config_mutex_ first
    reconfig_mutex_.lock();

    // Check the reconfig_flag_
    if (reconfig_msg_ != NULL) {
      Reconfigurate();
    }
    // Unlock the config_mutex_
    reconfig_mutex_.unlock();

    // case 0: Pull request from worker
    if (signal_type == 0) {
      cout << "Agent: Receive pull request from worker" << endl;
      parameters_ = *grad_memory_.Read();
      cout << "Agent: Pull size = " << parameters_.size << ": ";
      for (int32 i = 0; i < parameters_.size; i++) 
        cout << parameters_.keys[i] << " ";
      cout << endl;
      cout << "Agent: Try to Pull" << endl;
      Pull();
      cout << "Agent: Write parameters to memory" << endl;
      para_memory_.Write(&parameters_);
      cout << "Agent: parameters_.size = " << parameters_.size << endl;
      cout << "Agent: (key, value)s are as follows:" << endl;
      for (int32 i = 0; i < parameters_.size; i++) {
        cout << "(" << parameters_.keys[i] << ", " << parameters_.values[i]
             << ")" << ", ";
      }
      cout << endl;
      cout << "Agent: Signal to the worker" << endl;
      para_fifo_.Signal(2);
      cout << "Pull Done" << endl;
    } else if (signal_type == 1) {
    // case 1: Push request from worker
      cout << "Agent: Read gradients from memory" << endl;
      gradients_ = *grad_memory_.Read();
      cout << "Agent: gradients_.size = " << gradients_.size << endl;
      cout << "(key, value)s are as follows:" << endl;
      for (int32 i = 0; i < gradients_.size; i++) {
        cout << "(" << gradients_.keys[i] << ", " << gradients_.values[i]
             << ")" << ", ";
      }
      cout << endl;
      cout << "Agent: Try to Push" << endl;
      Push();
      epoch_num_++;
    } else {
    // case 2: Terminate request from worker
      cout << "Agent: Terminate" << endl;
      Message msg_send;
      msg_send.set_message_type(Message_MessageType_terminate);
      msg_send.set_send_id(local_id_);
      msg_send.set_recv_id(0);
      std::string msg_str;
      msg_send.SerializeToString(&msg_str);
      if (sender_->Send(0, msg_str) == -1) {
        LOG(INFO) << "Cannot send push message to master" << endl;
        LOG(ERROR) << "Cannot send push message to master";
      }
      sleep(10);
      cout << "Agent terminates its work" << endl;
      break;
    }
  }
  return true;
}

bool Agent::Push() {
  int32 start, end, server_id, size;
  Message msg_send;
  Message_RequestMessage* request_msg_ptr;
  std::string request_str;

  // Sort the key_value_list_ by the key, and then send them by blocks.
  cout << "Agent: Before SortKeyValue : " << endl;
  cout << "Agent: gradients_.size = " << gradients_.size << endl;
  cout << "(key, value)s are as follows:" << endl;
  for (int32 i = 0; i < gradients_.size; i++) {
    cout << "(" << gradients_.keys[i] << ", " << gradients_.values[i]
         << ")" << ", ";
  }
  cout << endl;
  
  SortKeyValue(gradients_.keys, gradients_.values, gradients_.size);
  cout << "Agent: After SortKeyValue : " << endl;
  cout << "Agent: gradients_.size = " << gradients_.size << endl;
  cout << "(key, value)s are as follows:" << endl;
  for (int32 i = 0; i < gradients_.size; i++) {
    cout << "(" << gradients_.keys[i] << ", " << gradients_.values[i]
         << ")" << ", ";
  }
  cout << endl;
  
  // Set the message type
  msg_send.set_message_type(Message_MessageType_request);

  // Set send_id
  msg_send.set_send_id(local_id_);

  // Divide key list and value list and send them to different serverss
  start = 0;
  size = gradients_.size;
  cout << "Agent: gradients_.size = " << gradients_.size << endl;
  while (start < size) {
    end = partition_.NextEnding(std::vector<int>(gradients_.keys,
                              gradients_.keys + gradients_.size), 
                              start, server_id);
    cout << "Agent_server_id_list" << endl;
    for (auto item : server_ids_) {
      cout << item << ", ";
    }
    cout << endl;
    cout << "Agent: start, end = " << start << ", " << end << endl;
    server_id = server_ids_[server_id];
    cout << "Agent: server_id = " << server_id << endl;
    msg_send.clear_request_msg();
    request_msg_ptr = new Message_RequestMessage();
    request_msg_ptr->set_request_type
                 (Message_RequestMessage_RequestType_key_value);                  
    request_msg_ptr->clear_keys();
    request_msg_ptr->clear_values();
    for (int32 i = start; i < end; i++) {
      request_msg_ptr->add_keys(gradients_.keys[i]);
      request_msg_ptr->add_values(gradients_.values[i]);
    }
    msg_send.set_allocated_request_msg(request_msg_ptr);
    msg_send.set_recv_id(server_id);
    msg_send.SerializeToString(&request_str);
    cout << "Agent: Send 'push' to server" << endl;
    if (sender_->Send(server_id, request_str) == -1) {
      LOG(INFO) << "Cannot send push message to server:" << server_id;
      LOG(ERROR) << "Cannot send push message to server:" << server_id;
    }

    start = end;
  }

  return true;
}

bool Agent::Pull() {
  // Agent will sort the key_list_, and send pull request to servers by blocks.
  // Then it will wait until it has received all the replies from the servers
  // the agent have requested to
  int32 start, end, server_id, size;
  Message msg_send_recv;
  Message_RequestMessage* request_msg_ptr;
  std::string msg_str;
  std::set<int32> server_set;

  // Sort the key_list_
  std::sort(parameters_.keys, parameters_.keys + parameters_.size);

  // Set the message type
  msg_send_recv.set_message_type(Message_MessageType_request);

  // Set send_id
  msg_send_recv.set_send_id(local_id_);

  // Divide key list and send them to different servers
  start = 0;
  size = parameters_.size;
  while (start < size) {
    end = partition_.NextEnding(std::vector<int>(parameters_.keys,
                              parameters_.keys + parameters_.size),
                              start, server_id);
    cout << "Agent: start, end = " << start << ", " << end << endl;
    server_id = server_ids_[server_id];
    cout << "Agent: server_id = " << server_id << endl;
    server_set.insert(server_id);
    msg_send_recv.clear_request_msg();
    request_msg_ptr = new Message_RequestMessage();
    request_msg_ptr->set_request_type(Message_RequestMessage_RequestType_key);
    request_msg_ptr->clear_keys();
    for (int32 i = start; i < end; i++) {
      request_msg_ptr->add_keys(parameters_.keys[i]);
    }
    msg_send_recv.set_allocated_request_msg(request_msg_ptr);
    msg_send_recv.set_recv_id(server_id);
    msg_send_recv.SerializeToString(&msg_str);
    cout << "Agent: Send 'pull' to server" << endl;
    if (sender_->Send(server_id, msg_str) == -1) {
      LOG(INFO) << "Cannot send pull message to server:" << server_id;
      LOG(ERROR) << "Cannot send pull message to server:" << server_id;
    }

    start = end;
  }

  // Receive parameters from servers
  // PS: Maybe I will add a timer for this loop. Beacuse I want to avoid
  // infinite loop caused by crashed server or servers.

  int32 cur = 0;
  parameters_.size = 0;
  
  cout << "Agent: Start waiting for server's response" << endl;
  cout << "Agent: server_set: " << server_set.size() << endl;
  for (auto item : server_set) {
    cout << item << endl;
  }
  while (!server_set.empty()) {
    if (receiver_->Receive(&msg_str) == -1) {
      cout << "Agent: Error in receiving message from servers" << endl;
      LOG(ERROR) << "Error in receiving message from servers";
    } else {
      msg_send_recv.ParseFromString(msg_str);
      cout << "Agent: Receive " << msg_send_recv.DebugString() << endl;
      
      // Ignore wrong messages
      // Check the message type
      cout << "Agent: Check the message from server" << endl;
      if (msg_send_recv.message_type() != Message_MessageType_request) {
        LOG(ERROR) << "Agent receives a message with wrong message_type";
        continue;
      }
      // Check the message content
      if (!msg_send_recv.has_request_msg()) {
        LOG(ERROR) << "Agent receives a message without request_message";
        continue;
      }
      // Check the message's send_id
      if (server_set.find(msg_send_recv.send_id()) == server_set.end()) {
        LOG(ERROR) << "Agent receives a message from an unknown sender";
        continue;
      }
      // Check the message's recv_id
      if (msg_send_recv.recv_id() != local_id_) {
        LOG(ERROR) << "Agent receives a message with a wrong recv_id";
        continue;
      }

      // Parse the request_msg
      // Check the request_msg's type
      Message_RequestMessage request_msg = msg_send_recv.request_msg();
      // PS: Maybe the agent will send a feedback message to server in the
      // future
      if (request_msg.request_type() !=
          Message_RequestMessage_RequestType_key_value) {
        LOG(ERROR) << "Agent receives a message with wrong request_type";
        continue;
      }
      cout << "Agent: Get response from server " << msg_send_recv.send_id() 
           << endl;
      server_set.erase(msg_send_recv.send_id());
      size = request_msg.keys_size();
      cout << "Agent: Receive " << size << " key_value pairs" << endl;
      cout << "Agent: cur = " <<  cur << endl;
      // PS: Maybe I will allocate stable space for part_keys and part_values,
      // if I know the maximal number of key-value pairs
      // Extract parameter keys from the message
      
      for (int32 i = 0; i < size; i++) {
        parameters_.keys[cur + i] = request_msg.keys(i);
        parameters_.values[cur + i] = request_msg.values(i);
      }
      /*
      int32* part_keys = new int32[size];
      request_msg.mutable_keys()->ExtractSubrange(0, size, part_keys);
      memcpy(parameters_.keys + cur * 4, part_keys, size * 4);
      delete[] part_keys;
      // Extract parameter values from the message
      float32* part_values = new float32[size];
      request_msg.mutable_values()->ExtractSubrange(0, size, part_values);
      memcpy(parameters_.values + cur * 4, part_values, size * 4);
      delete[] part_values;
      */
      cur += size;
    }
  }
  parameters_.size = cur;
  cout << "Agent: parameters_.size = " << parameters_.size << endl;
  return true;
}

void* Agent::HeartBeat(void* arg) {
  Agent* agent = reinterpret_cast<Agent*>(arg);
  std::unique_ptr<Communicator> hreceiver;

  hreceiver.reset(new ZmqCommunicator());
  if (hreceiver.get() == NULL) {
    cout << "Initialize hreceiver failed." << endl;
    return nullptr;
  }
  hreceiver->Initialize(64/* ring_size */, false, agent->listen_port_ + 1);

  Message send_msg, recv_msg;
  Message_HeartbeatMessage* hb_msg = new Message_HeartbeatMessage();
  std::string send_str, recv_str;

  hb_msg->set_is_live(true);
  hb_msg->set_agent_epoch_num(agent->epoch_num_);

  send_msg.set_message_type(Message_MessageType_heartbeat);
  send_msg.set_send_id(agent->local_id_);
  send_msg.set_allocated_heartbeat_msg(hb_msg);

  while (1) {
    if (hreceiver->Receive(&recv_str) == -1) {
      cout << "Error in receiving heartbeat from master" << endl;
    }
    recv_msg.ParseFromString(recv_str);
    cout << "Received heartbeat from master" << endl;
    cout << "Master send_id is " << recv_msg.send_id() << endl;

    // Check if there is a config message in the heartbeat
    if (recv_msg.has_config_msg()) {
      cout << "Receive a reconfig message from master" << endl;
      agent->reconfig_mutex_.lock();
      agent->reconfig_msg_ = new Message(recv_msg);
      agent->reconfig_mutex_.unlock();
//      {
//        lock_guard<mutex> guard(reconfig_mutex_);
//        agent->reconfig_msg_ = new Message(recv_msg);
//      }
    }

    // Config the send_msg

    send_msg.set_recv_id(recv_msg.send_id());
    hb_msg = new Message_HeartbeatMessage();
    hb_msg->set_is_live(true);
    hb_msg->set_agent_epoch_num(agent->epoch_num_);
    send_msg.set_allocated_heartbeat_msg(hb_msg);
    send_msg.SerializeToString(&send_str);

    if (agent->sender_->Send(0, send_str) == -1) {
      cout << "Cannot send a heartbeat to master" << endl;
    }
    cout << "Sent heartbeat to master" << endl;
  }

  return nullptr;
}

void Agent::Reconfigurate() {
  // 1.Reinitialization of agent field
  local_id_ = reconfig_msg_->recv_id();
  Message_ConfigMessage config_msg = reconfig_msg_->config_msg();
  agent_num_ = config_msg.worker_num();
  server_num_ = config_msg.server_num();
  key_range_  = config_msg.key_range();

  cout << "Reinitialization " << "local_id = " << local_id_
       << " agent_num_ = " << agent_num_ << " server_num_ = "
       << server_num_ << " key_range_ = " << key_range_ << endl;

  // 2.Refresh the server_ids_, master_ids_ and the sender_
  server_ids_.clear();
  for (int32 i = 0; i < server_num_; i++) {
    server_ids_.push_back(config_msg.server_id(i));}
  master_ids_.clear();
  for (int32 i = 0; i < config_msg.master_id_size(); ++i) {
    master_ids_.push_back(config_msg.master_id(i));
  }

  for (int32 i = 0; i < config_msg.node_ip_port_size(); i++) {
    if (!sender_->CheckIdAddr(i, config_msg.node_ip_port(i))) {
      sender_->DeleteId(i);
      sender_->AddIdAddr(i, config_msg.node_ip_port(i));
    }
  }

  // 4.Reinitialize the partition_.
  int32* part_vec = new int32[server_num_ + 1];
  config_msg.mutable_partition()->ExtractSubrange(0, server_num_ + 1,
                                                  part_vec);
  partition_.Finalize();
  partition_.Initialize(key_range_, server_num_, part_vec);
  delete[] part_vec;

  // 5.Set the epoch_num_ to 0
  epoch_num_ = 0;

  // 6.Set the reconfig_msg_ to NULL
  delete reconfig_msg_;
  reconfig_msg_ = NULL;
}

}  // namespace rpscc

