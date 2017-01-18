#ifndef DQN_HPP_
#define DQN_HPP_

#include <memory>
#include <random>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <HFO.hpp>
#include <caffe/caffe.hpp>
#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>
#include <boost/thread/barrier.hpp>
#include <mutex>

struct Action {
  hfo::action_t action;
  float arg1;
  float arg2;
};

namespace dqn {

constexpr auto kStateInputCount = 1;
constexpr auto kMinibatchSize = 32;
constexpr auto kHFOParams = 5;

using ActorOutput = std::vector<float>;
using StateData   = std::vector<float>;
using StateDataSp = std::shared_ptr<StateData>;
using InputStates = std::array<StateDataSp, kStateInputCount>;
using Transition  = std::tuple<InputStates,                   // 0 State
                               int,                           // 1 Task Identifier
                               ActorOutput,                   // 2 Action
                               float,                         // 3 Reward
                               float,                         // 4 OnPolicy Target
                               boost::optional<StateDataSp>>; // 5 Next-State
using SolverSp    = std::shared_ptr<caffe::Solver<float>>;
using NetSp       = boost::shared_ptr<caffe::Net<float>>;

// Layer Names
constexpr auto state_input_layer_name         = "state_input_layer";
constexpr auto action_input_layer_name        = "action_input_layer";
constexpr auto action_params_input_layer_name = "action_params_input_layer";
constexpr auto task_input_layer_name          = "task_input_layer";
constexpr auto target_input_layer_name        = "target_input_layer";
constexpr auto filter_input_layer_name        = "filter_input_layer";
constexpr auto q_values_layer_name            = "q_values_layer";
// Blob names
constexpr auto states_blob_name        = "states";
constexpr auto task_blob_name          = "task";
constexpr auto actions_blob_name       = "actions";
constexpr auto action_params_blob_name = "action_params";
constexpr auto targets_blob_name       = "target";
constexpr auto filter_blob_name        = "filter";
constexpr auto q_values_blob_name      = "q_values";
constexpr auto loss_blob_name          = "loss";
constexpr auto reward_blob_name        = "reward";

/**
 * Deep Q-Network
 */
class DQN {
public:
  DQN(caffe::SolverParameter& actor_solver_param,
      caffe::SolverParameter& critic_solver_param,
      caffe::SolverParameter& semantic_solver_param,
      std::string save_path, int state_size, int tid,
      int num_discrete_actions, int num_continuous_actions);
  ~DQN();

  // Benchmark the speed of updates
  void Benchmark(int iterations=1000);

  // Loading methods
  void RestoreActorSolver(const std::string& actor_solver);
  void RestoreCriticSolver(const std::string& critic_solver);
  void RestoreSemanticSolver(const std::string& semantic_solver);
  void LoadActorWeights(const std::string& actor_model_file);
  void LoadCriticWeights(const std::string& critic_weights);
  void LoadSemanticWeights(const std::string& semantic_weights);
  void LoadReplayMemory(const std::string& filename);

  // Snapshot the model/solver/replay memory. Produces files:
  // snapshot_prefix_iter_N.[caffemodel|solverstate|replaymem]. Optionally
  // removes snapshots with same prefix but lower iteration.
  void Snapshot();
  void Snapshot(const std::string& snapshot_prefix, bool remove_old=false,
                bool snapshot_memory=true);

  ActorOutput GetRandomActorOutput();
  // The communication is not randomized
  void RandomizeNonCommActions(ActorOutput& actor_output);

  // Select an action using epsilon-greedy action selection.
  ActorOutput SelectAction(const InputStates& input_states,
                           const float& task_id,
                           const double& epsilon);

  // Select a batch of actions using epsilon-greedy action selection.
  std::vector<ActorOutput> SelectActions(const std::vector<InputStates>& states_batch,
                                         const std::vector<float>& task_batch,
                                         double epsilon);

  // Converts an ActorOutput into an action by samping over discrete actions
  Action SampleAction(const ActorOutput& actor_output);

  // Converts an ActorOutput into an action by maxing over discrete actions
  Action GetAction(const ActorOutput& actor_output);

  std::string PrintActorOutput(const ActorOutput& actor_output);
  std::string PrintActorOutput(const float* actions, const float* params);

  // Evaluate a state-action, returning the q-value.
  float EvaluateAction(const InputStates& input_states,
                       const float& task_id,
                       const ActorOutput& action);

  // Fills hear_msg with other players say messages
  void GetHearFeatures(hfo::HFOEnvironment& env, std::vector<float>& hear_msg);

  // Returns the outgoing message to be said in-game
  std::string GetSayMsg(const ActorOutput& actor_output);

  // Extract a message from the semantic network
  std::string GetSemanticMsg(const InputStates& last_states,
                             const float& task_id);

  // Add a transition to replay memory
  void AddTransition(const Transition& transition);
  void AddTransitions(const std::vector<Transition>& transitions);

  // Computes a tabular Q-Value for each transition
  void LabelTransitions(std::vector<Transition>& transitions);

  // Update the model(s)
  void Update();

  void SynchronizedUpdate(boost::barrier& barrier,
                          std::vector<int>& transitions,
                          std::vector<float*>& gradients,
                          std::vector<Transition>& episode);

  void UpdateSemanticNet(std::deque<Transition>* other_memory);

  // Get access to the replay memory
  std::deque<Transition>* getMemory() { return replay_memory_.get(); }

  // Clear the replay memory
  void ClearReplayMemory() { replay_memory_->clear(); }

  // Save the replay memory to a gzipped compressed file
  void SnapshotReplayMemory(const std::string& filename);

  // Get the current size of the replay memory
  int memory_size() const { return replay_memory_->size(); }

  // Share the parameters in a layer. Owner keeps the params, slave loses them
  void ShareLayer(caffe::Layer<float>& param_owner,
                  caffe::Layer<float>& param_slave);

  // Share parameters between DQNs
  void ShareParameters(DQN& other,
                       int num_actor_layers_to_share,
                       int num_critic_layers_to_share);

  // Free's the replay memory of other, which now points to our own replay mem
  void ShareReplayMemory(DQN& other);

  // Return the current iteration of the solvers
  int min_iter() const { return std::min(actor_iter(), critic_iter()); }
  int max_iter() const { return std::max(actor_iter(), critic_iter()); }
  int critic_iter() const { return critic_solver_->iter(); }
  int actor_iter() const { return actor_solver_->iter(); }
  int semantic_iter() const { return semantic_solver_->iter(); }
  int state_size() const { return state_size_; }
  const std::string& save_path() const { return save_path_; }
  int unum() const { return unum_; }
  void set_unum(int unum) { unum_ = unum; }

protected:
  // Initialize DQN. Called by the constructor
  void Initialize();

  // Update both the actor and critic.
  std::pair<float, float> UpdateActorCritic(const std::vector<int>& transitions);

  // Synchronized update between two agents where communication
  // gradients are exchanged
  std::pair<float, float> SyncUpdateActorCritic(const std::vector<int>& transitions,
                                                boost::barrier& barrier,
                                                std::vector<float*>& gradients);
  // Approximate (quick) version of the above update. Does not resepct
  // the delay in communication.
  std::pair<float, float> ApproxSyncUpdateActorCritic(const std::vector<int>& transitions,
                                                      boost::barrier& barrier,
                                                      std::vector<float*>& gradients);

  // DIAL Update
  std::pair<float, float> DialUpdate(std::vector<Transition>& episode,
                                     boost::barrier& barrier,
                                     std::vector<float*>& gradients);


  // Update the semantic net
  float UpdateSemanticNet(const std::vector<int>& transitions,
                          std::deque<Transition>* other_memory);

  // Randomly sample the replay memory n-times, returning transition indexes
  std::vector<int> SampleTransitionsFromMemory(int n);
  // Randomly sample the replay memory n-times returning input_states
  std::vector<InputStates> SampleStatesFromMemory(int n);

  // Clone the network and store the result in clone_net_
  void CloneNet(NetSp& net_from, NetSp& net_to);
  // Update the parameters of net_to towards net_from.
  // net_to = tau * net_from + (1 - tau) * net_to
  void SoftUpdateNet(NetSp& net_from, NetSp& net_to, float tau);

  // Given input states, use the actor network to select an action.
  ActorOutput SelectActionGreedily(caffe::Net<float>& actor,
                                   const InputStates& last_states,
                                   const float& task_id);

  // Given a batch of input states, return a batch of selected actions.
  std::vector<ActorOutput> SelectActionGreedily(
      caffe::Net<float>& actor,
      const std::vector<InputStates>& states_batch,
      const std::vector<float>& task_batch);

  std::vector<ActorOutput> getActorOutput(caffe::Net<float>& actor,
                                          int batch_size,
                                          std::string actions_blob_name);

  // Runs forward on critic to produce q-values. Actions inferred by actor.
  std::vector<float> CriticForwardThroughActor(
      caffe::Net<float>& critic, caffe::Net<float>& actor,
      const std::vector<InputStates>& states_batch,
      const std::vector<float>& task_batch);
  std::vector<float> CriticForwardThroughActor(
      caffe::Net<float>& critic, caffe::Net<float>& actor,
      const std::vector<InputStates>& states_batch,
      const std::vector<float>& task_batch,
      float* teammate_comm_actions);

  // Runs forward on critic to produce q-values.
  std::vector<float> CriticForward(caffe::Net<float>& critic,
                                   const std::vector<InputStates>& states_batch,
                                   const std::vector<float>& task_batch,
                                   const std::vector<ActorOutput>& action_batch);

  std::vector<float> CriticForward(caffe::Net<float>& critic,
                                   const std::vector<InputStates>& states_batch,
                                   const std::vector<float>& task_batch,
                                   float* teammate_comm_actions,
                                   const std::vector<ActorOutput>& action_batch);

  std::vector<float> SemanticForward(caffe::Net<float>& semantic,
                                     const std::vector<InputStates>& states_batch,
                                     const std::vector<float>& task_batch);

  // Input data into the State/Target/Filter layers of the given
  // net. This must be done before forward is called.
  void InputDataIntoLayers(caffe::Net<float>& net,
                           float* states_input,
                           float* task_input,
                           float* actions_input,
                           float* action_params_input,
                           float* target_input,
                           float* filter_input);

protected:
  caffe::SolverParameter actor_solver_param_;
  caffe::SolverParameter critic_solver_param_;
  caffe::SolverParameter semantic_solver_param_;
  const int replay_memory_capacity_;
  const double gamma_;
  std::shared_ptr<std::deque<Transition> > replay_memory_;
  SolverSp actor_solver_;
  NetSp actor_net_; // The actor network used for continuous action evaluation.
  SolverSp critic_solver_;
  NetSp critic_net_;  // The critic network used for giving q-value of a continuous action;
  SolverSp semantic_solver_;
  NetSp semantic_net_;
  // NetSp semantic_target_net_;
  NetSp critic_target_net_; // Clone of critic net. Used to generate targets.
  NetSp actor_target_net_; // Clone of the actor net. Used to generate targets.
  std::mt19937 random_engine;
  float smoothed_critic_loss_, smoothed_actor_loss_, smoothed_semantic_loss_;
  int last_snapshot_iter_;
  std::string save_path_;

  const int state_size_; // Number of state features
  const int state_input_data_size_;

  const int kActionSize; // Number of discrete actions
  const int kActionParamSize; // Number of continuous actions
  const int kActionInputDataSize;
  const int kActionParamsInputDataSize;
  const int kTargetInputDataSize;

  int tid_;
  int unum_;
};

caffe::NetParameter CreateActorNet(
    int state_size, int num_discrete_actions, int num_continuous_actions, int num_tasks);
caffe::NetParameter CreateCriticNet(
    int state_size, int num_discrete_actions, int num_continuous_actions, int num_tasks);
caffe::NetParameter CreateSemanticNet(
    int state_size, int num_discrete_actions, int num_continuous_actions,
    int num_tasks, int message_size);
/**
 * Returns a vector of filenames matching a given regular expression.
 */
std::vector<std::string> FilesMatchingRegexp(const std::string& regexp);

// Removes all files matching a given regular expression
void RemoveFilesMatchingRegexp(const std::string& regexp);

/**
 * Removes snapshots matching regexp that have an iteration less than
 * min_iter.
 */
void RemoveSnapshots(const std::string& regexp, int min_iter);

/**
 * Look for the latest snapshot to resume from. Returns a string
 * containing the path to the .solverstate. Returns empty string if
 * none is found. Will only return if the snapshot contains all of:
 * .solverstate,.caffemodel,.replaymemory
 */
void FindLatestSnapshot(const std::string& snapshot_prefix,
                        std::string& actor_snapshot,
                        std::string& critic_snapshot,
                        std::string& semantic_snapshot,
                        std::string& memory_snapshot,
                        bool load_solver);

/**
 * Look for the best HiScore matching the given snapshot prefix
 */
int FindHiScore(const std::string& snapshot_prefix);

template<typename Dtype>
std::string PrintVector(const std::vector<Dtype>& v) {
  std::string s;
  for (int i=0; i<v.size(); ++i) {
    s.append(std::to_string(v[i]) + " ");
  }
  return s;
}

template<typename Dtype>
std::string PrintVector(Dtype* v, int num) {
  std::string s;
  for (int i=0; i<num; ++i) {
    s.append(std::to_string(v[i]) + " ");
  }
  return s;
}

} // namespace dqn

#endif /* DQN_HPP_ */
