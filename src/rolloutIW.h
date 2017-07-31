// (c) 2017 Blai Bonet

#ifndef ROLLOUT_IW_H
#define ROLLOUT_IW_H

#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <ale_interface.hpp>

#include "planner.h"
#include "node.h"
#include "screen.h"
#include "utils.h"

struct RolloutIWPlanner : Planner {
    ALEInterface &sim_;
    ALEState initial_sim_state_;
    ActionVect minimal_actions_;
    size_t minimal_actions_size_;

    const size_t frameskip_;
    const float budget_secs_per_decision_;
    const int screen_features_type_;
    const bool feature_stratification_;
    const size_t num_tracked_atoms_;
    const size_t max_depth_;
    const size_t max_rep_;;
    const float discount_;
    const float alpha_;
    const bool debug_;

    mutable size_t simulator_calls_;
    mutable size_t num_rollouts_;
    mutable size_t num_expansions_;
    mutable size_t num_cases_[4];
    mutable float total_time_;
    mutable float simulator_time_;
    mutable float reset_time_;
    mutable float get_set_state_time_;
    mutable float update_novelty_time_;
    mutable float get_atoms_time_;
    mutable float novel_atom_time_;
    mutable float expand_time_;

    RolloutIWPlanner(ALEInterface &sim,
                     size_t frameskip,
                     float budget_secs_per_decision,
                     int screen_features_type,
                     bool feature_stratification,
                     size_t num_tracked_atoms,
                     size_t max_depth,
                     size_t max_rep,
                     float discount,
                     float alpha,
                     bool debug = false)
      : sim_(sim),
        frameskip_(frameskip),
        budget_secs_per_decision_(budget_secs_per_decision),
        screen_features_type_(screen_features_type),
        feature_stratification_(feature_stratification),
        num_tracked_atoms_(num_tracked_atoms),
        max_depth_(max_depth),
        max_rep_(max_rep),
        discount_(discount),
        alpha_(alpha),
        debug_(debug) {
        //static_assert(std::numeric_limits<float>::is_iec559, "IEEE 754 required");
        assert(sim_.getInt("frame_skip") == frameskip_);
        minimal_actions_ = sim_.getMinimalActionSet();
        minimal_actions_size_ = minimal_actions_.size();
        reset_game(sim_);
        get_state(sim_, initial_sim_state_);
    }

    virtual std::string name() const {
        return std::string("rollout(")
          + "frameskip=" + std::to_string(frameskip_)
          + ",budget=" + std::to_string(budget_secs_per_decision_)
          + ",features=" + std::to_string(screen_features_type_)
          + ",stratification=" + std::to_string(feature_stratification_)
          + ",max-depth=" + std::to_string(max_depth_)
          + ",max-rep=" + std::to_string(max_rep_)
          + ",discount=" + std::to_string(discount_)
          + ",alpha=" + std::to_string(alpha_)
          + ",debug=" + std::to_string(debug_)
          + ")";
    }

    virtual Action random_action() const {
        return minimal_actions_[lrand48() % minimal_actions_size_];
    }

    virtual Node* get_branch(ALEInterface &env,
                             const std::vector<Action> &prefix,
                             Node *root,
                             float last_reward,
                             std::deque<Action> &branch) const {
        assert(!prefix.empty());
        std::cout << "**** get branch ****" << std::endl;
        std::cout << "prefix: sz=" << prefix.size() << ", actions=";
        print_prefix(std::cout, prefix);
        std::cout << std::endl;
        std::cout << "input:"
                  << " #nodes=" << (root == nullptr ? 0 : root->num_nodes())
                  << ", height=" << (root == nullptr ? "na" : std::to_string(root->height_))
                  << std::endl;

        // reset stats and start timer
        reset_stats();
        float start_time = Utils::read_time_in_seconds();

        // novelty table and other vars
        std::vector<int> novelty_table(num_tracked_atoms_, std::numeric_limits<int>::max());
        std::pair<bool, bool> seen_rewards(false, false);

        // construct root node
        assert((root == nullptr) || (root->action_ == prefix.back()));
        if( root == nullptr ) {
            Node *root_parent = new Node(nullptr, PLAYER_A_NOOP, -1);
            root_parent->state_ = new ALEState;
            apply_prefix(sim_, initial_sim_state_, prefix, root_parent->state_);
            root = new Node(root_parent, prefix.back(), 0);
        }
        assert(root->parent_ != nullptr);
        root->parent_->parent_ = nullptr;

        // clear solved labels and normalize depths
        root->clear_solved_labels();
        root->parent_->solved_ = false;
        root->normalize_depth();

        // construct lookahead tree
        float elapsed_time = Utils::read_time_in_seconds() - start_time;
        while( !root->solved_ && (elapsed_time < budget_secs_per_decision_) ) {
            if( debug_ ) std::cout << '.' << std::flush;
            rollout(prefix, root, max_depth_, max_rep_, alpha_, novelty_table, seen_rewards);
#if 0
            if( seen_rewards.first && !seen_rewards.second ) {
                root->backup_values(discount_);
                assert(root->value_ > 0);
                const Node *tip_node = root->best_tip_node(discount_);
                bool good_tip = do_random_lookahead_below_node(tip_node, 35, 50);
                if( good_tip ) {
                    std::cout << "%" << std::flush;
                    break;
                } else {
                    const_cast<Node*>(tip_node)->reward_ = -alpha_;
                }
            }
#endif
            elapsed_time = Utils::read_time_in_seconds() - start_time;
        }
        if( debug_ ) std::cout << std::endl;

        // backup values and calculate heights
        assert(!root->children_.empty());
        root->backup_values(discount_);
        root->calculate_height();
        assert(!seen_rewards.first || seen_rewards.second || (root->value_ > 0));

        // print info about root node
        if( true || debug_ ) {
            std::cout << Utils::green()
                      << "root:"
                      << " solved=" << root->solved_
                      << ", value=" << root->value_
                      << ", imm-reward=" << root->reward_
                      << ", children=[";
            for( size_t k = 0; k < root->children_.size(); ++k )
                std::cout << root->children_[k]->value_ << ":" << root->children_[k]->action_ << " ";
            std::cout << "]" << Utils::normal() << std::endl;
        }

        // compute branch
        if( root->value_ > 0 ) {
            root->best_branch(branch, discount_);
#if 0
            size_t branch_size = branch.size();
            do {
                float branch_value_before_tip_lookahead = root->value_;
                //do_lookahead_at_branch_tip(root, sim_state, branch, 10);
                float branch_value_after_tip_lookahead = root->backup_values_along_branch(branch, discount_);
                std::cout << "values: branch-before=" << branch_value_before_tip_lookahead
                          << ", branch-after=" << branch_value_after_tip_lookahead
                          << ", root=" << root->value_
                          << std::endl;
                assert(root->value_ >= branch_value_after_tip_lookahead);
                if( (branch_value_after_tip_lookahead > 0) || (root->value_ <= 0) ) {
                    if( branch_value_after_tip_lookahead > 0 ) {
                        assert(root->value_ > 0);
                        while( branch.size() > branch_size )
                            branch.pop_back();
                    }
                    break;
                } else {
                    assert(root->value_ > 0);
                    branch.clear();
                    root->best_branch(branch, discount_);
                    branch_size = branch.size();
                }
            } while( root->value_ > 0 );
#endif
        }

        if( root->value_ == 0 ) {
            root->longest_zero_value_branch(branch);
            size_t n = branch.size() >> 1;
            n = n == 0 ? 1 : n;
            while( branch.size() > n )
                branch.pop_back();
        } else if( root->value_ < 0 ) {
            root->best_branch(branch, discount_);
        }
        assert(!branch.empty());
        if( true || debug_ ) {
            std::cout << "branch:"
                      << " value=" << root->value_
                      << ", size=" << branch.size()
                      << ", actions:"
                      << std::endl;
            root->print_branch(std::cout, branch);
        }

        // stop timer and print stats
        total_time_ = Utils::read_time_in_seconds() - start_time;
        if( true || debug_ )
            print_stats(std::cout, *root, novelty_table);

        // return root node
        return root;
    }

    void rollout(const std::vector<Action> &prefix,
                 Node *root,
                 size_t max_depth,
                 size_t max_rep,
                 float alpha,
                 std::vector<int> &novelty_table,
                 std::pair<bool, bool> &seen_rewards) const {
        ++num_rollouts_;
        seen_rewards = std::pair<bool, bool>(false, false);

        // apply prefix
        //apply_prefix(sim_, initial_sim_state_, prefix);

        // update root info
        if( !root->is_info_valid_ )
            update_info(root, alpha);

        // perform rollout
        Node *node = root;
        while( !node->solved_ ) {
            assert(node->is_info_valid_);

            // if first time at this node, expand node
            if( node->children_.empty() ) {
                if( node->frame_rep_ == 0 ) {
                    ++num_expansions_;
                    float start_time = Utils::read_time_in_seconds();
                    node->expand(minimal_actions_);
                    expand_time_ += Utils::read_time_in_seconds() - start_time;
                } else {
                    assert((node->parent_ != nullptr) && (screen_features_type_ > 0));
                    node->expand(node->action_);
                }
                assert(!node->children_.empty());
            }

            // pick random unsolved child
            size_t num_unsolved_children = 0;
            for( size_t k = 0; k < node->children_.size(); ++k )
                num_unsolved_children += node->children_[k]->solved_ ? 0 : 1;
            assert(num_unsolved_children > 0);
            size_t index = lrand48() % num_unsolved_children;
            for( size_t k = 0; k < node->children_.size(); ++k ) {
                if( !node->children_[k]->solved_ ) {
                    if( index == 0 ) {
                        node = node->children_[k];
                        break;
                    }
                    --index;
                }
            }

            // update info
            if( !node->is_info_valid_ )
                update_info(node, alpha);

            // if terminal, label as solved and terminate rollout
            if( node->terminal_ ) {
                node->visited_ = true;
                assert(node->children_.empty());
                node->solve_and_backpropagate_label();
                //std::cout << "T[reward=" << node->reward_ << "]" << std::flush;
                break;
            }

            // verify repetitions of feature atoms (screen mode)
            if( node->frame_rep_ > max_rep ) {
                node->visited_ = true;
                //node->terminal_ = true;
                //node->reward_ = -10;
                assert(node->children_.empty());
                node->solve_and_backpropagate_label();
                //std::cout << "R" << std::flush;
                break;
            } else if( node->frame_rep_ > 0 ) {
                node->visited_ = true;
                //std::cout << "r" << std::flush;
                continue;
            }

            // report non-zero rewards
            if( node->reward_ > 0 ) {
                seen_rewards.first = true;
                //std::cout << Utils::yellow() << "+" << Utils::normal() << std::flush;
            } else if( node->reward_ < 0 ) {
                seen_rewards.second = true;
                //std::cout << "-" << std::flush;
            }

            // calculate novelty
            int atom = get_novel_atom(node->depth_, node->feature_atoms_, novelty_table);
            assert((atom >= 0) && (atom < num_tracked_atoms_));

            // five cases
            if( node->depth_ > max_depth ) {
                node->visited_ = true;
                assert(node->children_.empty());
                node->solve_and_backpropagate_label();
                //std::cout << "D" << std::flush;
                break;
            } else if( novelty_table[atom] > node->depth_ ) { // novel => not(visited)
                //assert(!node->visited_);
                if( !node->visited_ ) {
                    ++num_cases_[0];
                    node->visited_ = true;
                    update_novelty_table(node->depth_, node->feature_atoms_, novelty_table);
                    //std::cout << Utils::green() << "n" << Utils::normal() << std::flush;
                }
                continue;
            } else if( !node->visited_ && (novelty_table[atom] <= node->depth_) ) { // not(novel) and not(visited) => PRUNE
                ++num_cases_[1];
                node->visited_ = true;
                assert(node->children_.empty());
                node->solve_and_backpropagate_label();
                //std::cout << "x" << node->depth_ << std::flush;
                break;
            } else if( node->visited_ && (novelty_table[atom] < node->depth_) ) { // not(novel) and visited => PRUNE
                ++num_cases_[2];
                node->remove_children();
                node->reward_ = -std::numeric_limits<float>::infinity();
                node->solve_and_backpropagate_label();
                //std::cout << "X" << node->depth_ << std::flush;
                break;
            } else { // optimal and visited => CONTINUE
                assert(node->visited_ && (novelty_table[atom] == node->depth_));
                ++num_cases_[3];
                //std::cout << "c" << std::flush;
                continue;
            }
        }
    }

    void update_info(Node *node, float alpha) const {
        assert(!node->is_info_valid_);
        assert(node->state_ == nullptr);
        assert((node->parent_ != nullptr) && (node->parent_->state_ != nullptr));
        set_state(sim_, *node->parent_->state_);
        node->reward_ = call_simulator(sim_, node->action_);
        node->terminal_ = terminal_state(sim_);
        node->state_ = new ALEState;
        get_state(sim_, *node->state_);
        if( node->reward_ < 0 ) node->reward_ *= alpha;
        get_atoms(node);
        node->ale_lives_ = get_lives(sim_);
        if( (node->parent_ != nullptr) && (node->parent_->ale_lives_ != -1) ) {
            if( node->ale_lives_ < node->parent_->ale_lives_ ) {
                node->reward_ = -10 * alpha;
                //std::cout << "L" << std::flush;
            }
        }
        node->is_info_valid_ = true;
    }

#if 0
    bool do_random_lookahead_below_node(const Node *node, size_t num_random_probes, size_t max_depth_random_probe) const {
        assert(node->children_.empty());
        ALEState node_state;
        get_state(sim_, node_state);
        for( size_t k = 0; k < num_random_probes; ++k ) {
            set_state(sim_, node_state);
            for( size_t depth = 0; depth < max_depth_random_probe; ++depth ) {
                Action action = random_action();
                float reward = call_simulator(sim_, action);
                if( reward < 0 ) return false;
            }
        }
        return true;
    }

    void do_lookahead_at_branch_tip(Node *node, const ALEState &sim_state, const std::deque<Action> &branch, size_t max_depth, size_t max_rep, float alpha) const {
        ALEState tip_state = sim_state;
        set_state(sim_, tip_state);

        // go down to tip node in branch
        for( size_t index = 0; node->children_.empty(); ++index ) {
            assert(index < branch.size());
            const Action &action = branch[index];
            call_simulator(sim_, action);
            for( size_t k = 0; k < node->children_.size(); ++k ) {
                if( node->children_[k]->action_ == action ) {
                    node = node->children_[k];
                    break;
                }
            }
        }

        // perform extra look ahead at tip to see whether positive reward can be assured
        std::vector<Action> empty_prefix;
        std::pair<bool, bool> seen_rewards(false, false);
        std::map<std::pair<std::string, Action>, std::string> seen_transitions;
        std::vector<int> novelty_table(num_tracked_atoms_, std::numeric_limits<int>::max());
        while( !node->solved_ ) {
            if( true || debug_ ) std::cout << '#' << std::flush;
            set_state(sim_, tip_state);
            rollout(empty_prefix, node, max_depth, max_rep, alpha, novelty_table, seen_rewards, seen_transitions);
        }
        if( true || debug_ ) std::cout << std::endl;
    }
#endif

    void get_atoms(const Node *node) const {
        assert(node->feature_atoms_.empty());
        if( screen_features_type_ == 0 ) { // RAM mode
            get_atoms_from_ram(node);
        } else {
            get_atoms_from_screen(node);
            if( (node->parent_ != nullptr) && (node->parent_->feature_atoms_ == node->feature_atoms_) ) {
                node->frame_rep_ = node->parent_->frame_rep_ + frameskip_;
                assert(node->children_.empty());
            }
        }
        assert((node->frame_rep_ == 0) || (screen_features_type_ > 0));
    }
    void get_atoms_from_ram(const Node *node) const {
        assert(node->feature_atoms_.empty());
        node->feature_atoms_ = std::vector<int>(128, 0);
        float start_time = Utils::read_time_in_seconds();
        const ALERAM &ram = get_ram(sim_);
        for( size_t k = 0; k < 128; ++k ) {
            node->feature_atoms_[k] = (k << 8) + ram.get(k);
            assert((k == 0) || (node->feature_atoms_[k] > node->feature_atoms_[k-1]));
        }
        get_atoms_time_ += Utils::read_time_in_seconds() - start_time;
    }
    void get_atoms_from_screen(const Node *node) const {
        assert(node->feature_atoms_.empty());
        float start_time = Utils::read_time_in_seconds();
        if( (screen_features_type_ < 3) || (node->parent_ == nullptr) ) {
            MyALEScreen screen(sim_, screen_features_type_, &node->feature_atoms_);
        } else {
            assert((screen_features_type_ == 3) && (node->parent_ != nullptr));
            MyALEScreen screen(sim_, screen_features_type_, &node->feature_atoms_, &node->parent_->feature_atoms_);
        }
        get_atoms_time_ += Utils::read_time_in_seconds() - start_time;
    }

    int get_novel_atom(size_t depth, const std::vector<int> &feature_atoms, const std::vector<int> &novelty_table) const {
        float start_time = Utils::read_time_in_seconds();
        for( size_t k = 0; k < feature_atoms.size(); ++k ) {
            assert(feature_atoms[k] < novelty_table.size());
            if( novelty_table[feature_atoms[k]] > depth ) {
                novel_atom_time_ += Utils::read_time_in_seconds() - start_time;
                return feature_atoms[k];
            }
        }
        for( size_t k = 0; k < feature_atoms.size(); ++k ) {
            if( novelty_table[feature_atoms[k]] == depth ) {
                novel_atom_time_ += Utils::read_time_in_seconds() - start_time;
                return feature_atoms[k];
            }
        }
        novel_atom_time_ += Utils::read_time_in_seconds() - start_time;
        assert(novelty_table[feature_atoms[0]] < depth);
        return feature_atoms[0];
    }

    void update_novelty_table(size_t depth, const std::vector<int> &feature_atoms, std::vector<int> &novelty_table) const {
        float start_time = Utils::read_time_in_seconds();
        for( size_t k = 0; k < feature_atoms.size(); ++k ) {
            assert((feature_atoms[k] >= 0) && (feature_atoms[k] < novelty_table.size()));
            if( depth < novelty_table[feature_atoms[k]] )
                novelty_table[feature_atoms[k]] = depth;
        }
        update_novelty_time_ += Utils::read_time_in_seconds() - start_time;
    }

    size_t num_entries(const std::vector<int> &novelty_table) const {
        assert(novelty_table.size() == num_tracked_atoms_);
        size_t n = 0;
        for( size_t k = 0; k < novelty_table.size(); ++k )
            n += novelty_table[k] < std::numeric_limits<int>::max();
        return n;
    }

    float call_simulator(ALEInterface &ale, Action action) const {
        ++simulator_calls_;
        float start_time = Utils::read_time_in_seconds();
        //int frame_number = ale.getFrameNumber();
        float reward = ale.act(action);
        //assert(ale.getFrameNumber() == frame_number + 5);
        assert(reward != -std::numeric_limits<float>::infinity());
        simulator_time_ += Utils::read_time_in_seconds() - start_time;
        return reward;
    }

    void apply_prefix(ALEInterface &ale, const ALEState &initial_state, const std::vector<Action> &prefix, ALEState *last_state = nullptr) const {
        assert(!prefix.empty());
        reset_game(ale);
        set_state(ale, initial_state);
        for( size_t k = 0; k < prefix.size(); ++k ) {
            if( (last_state != nullptr) && (1 + k == prefix.size()) )
                get_state(ale, *last_state);
            call_simulator(ale, prefix[k]);
        }
    }

    int get_lives(ALEInterface &ale) const {
        return ale.lives();
    }

    bool terminal_state(ALEInterface &ale) const {
        return ale.game_over();
    }

    void get_state(ALEInterface &ale, ALEState &ale_state) const {
        float start_time = Utils::read_time_in_seconds();
        //ale_state = ale.cloneSystemState(); // CHECK
        ale_state = ale.cloneState();
        get_set_state_time_ += Utils::read_time_in_seconds() - start_time;
    }

    void set_state(ALEInterface &ale, const ALEState &ale_state) const {
        float start_time = Utils::read_time_in_seconds();
        //ale.restoreSystemState(ale_state); // CHECK
        ale.restoreState(ale_state);
        get_set_state_time_ += Utils::read_time_in_seconds() - start_time;
    }
    void set_state(ALEInterface &ale, const ALEState &ale_state, Action action) const {
        set_state(ale, ale_state);
        call_simulator(ale, action);
    }

    void reset_game(ALEInterface &ale) const {
        float start_time = Utils::read_time_in_seconds();
        ale.reset_game();
        reset_time_ += Utils::read_time_in_seconds() - start_time;
    }

    const ALERAM& get_ram(ALEInterface &ale) const {
        return ale.getRAM();
    }
    void get_ram(ALEInterface &ale, std::string &ram_str) const {
        ram_str = std::string(256, '0');
        const ALERAM &ale_ram = get_ram(ale);
        for( size_t k = 0; k < 128; ++k ) {
            byte_t byte = ale_ram.get(k);
            ram_str[2 * k] = "01234567890abcdef"[byte >> 4];
            ram_str[2 * k + 1] = "01234567890abcdef"[byte & 0xF];
        }
    }

    void reset_stats() const {
        simulator_calls_ = 0;
        num_rollouts_ = 0;
        num_expansions_ = 0;
        num_cases_[0] = 0;
        num_cases_[1] = 0;
        num_cases_[2] = 0;
        num_cases_[3] = 0;
        total_time_ = 0;
        simulator_time_ = 0;
        reset_time_ = 0;
        get_set_state_time_ = 0;
        update_novelty_time_ = 0;
        get_atoms_time_ = 0;
        novel_atom_time_ = 0;
        expand_time_ = 0;
    }

    void print_stats(std::ostream &os, const Node &root, const std::vector<int> &novelty_table) const {
        os << Utils::red()
           << "stats:"
           << " #rollouts=" << num_rollouts_
           << ", #entries=" << num_entries(novelty_table) << "/" << novelty_table.size()
           << ", #nodes=" << root.num_nodes()
           << ", #tips=" << root.num_tip_nodes()
           << ", height=[" << root.height_ << ":";

        for( size_t k = 0; k < root.children_.size(); ++k )
            os << root.children_[k]->height_ << " ";
        os << "]";

        os << ", #expansions=" << num_expansions_
           << ", #cases=[" << num_cases_[0] << "," << num_cases_[1] << "," << num_cases_[2] << "," << num_cases_[3] << "]"
           << ", #sim=" << simulator_calls_
           << ", total-time=" << total_time_
           << ", simulator-time=" << simulator_time_
           << ", reset-time=" << reset_time_
           << ", get/set-state-time=" << get_set_state_time_
           << ", expand-time=" << expand_time_
           << ", update-novelty-time=" << update_novelty_time_
           << ", get-atoms-time=" << get_atoms_time_
           << ", novel-atom-time=" << novel_atom_time_
           << Utils::normal() << std::endl;
    }

    void print_prefix(std::ostream &os, const std::vector<Action> &prefix) const {
        os << "[";
        for( size_t k = 0; k < prefix.size(); ++k )
            os << prefix[k] << ",";
        os << "]" << std::flush;
    }
};

#endif
