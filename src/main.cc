// (c) 2017 Blai Bonet

#include <iostream>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/tokenizer.hpp>
#include <ale_interface.hpp>

#include "planner.h"
#include "rolloutIW.h"
#include "utils.h"

#ifdef __USE_SDL
  #include <SDL.h>
#endif

using namespace std;
namespace fs = boost::filesystem;
namespace po = boost::program_options;

vector<pixel_t> MyALEScreen::background_;
size_t MyALEScreen::num_background_pixels_;
ActionVect MyALEScreen::minimal_actions_;
size_t MyALEScreen::minimal_actions_size_;


pair<float, pair<size_t, size_t> >
run_trial(ALEInterface &env, ostream &logos, const Planner &planner, bool execute_single_action, size_t frameskip, size_t max_execution_length_in_frames, vector<Action> &prefix) {
    assert(prefix.empty());

    env.reset_game();
    deque<Action> branch;
    prefix.push_back(planner.random_action());
    Node *node = nullptr;

    size_t decision = 0, frame = 0;
    float last_reward = env.act(prefix.back());
    float total_reward = last_reward;
    for( ; !env.game_over() && (frame < max_execution_length_in_frames); frame += frameskip ) {
        // if empty branch, get branch
        if( branch.empty() ) {
            ++decision;
            node = planner.get_branch(env, prefix, node, last_reward, branch);
            if( branch.empty() ) {
                logos << Utils::error() << "no more available actions!" << endl;
                break;
            } else {
                logos << "branch: len=" << branch.size() << ", actions=[";
                for( size_t j = 0; j < branch.size(); ++j )
                    logos << branch[j] << ",";
                logos << "]" << endl;
            }
        }

        // select action to apply
        Action action = branch.front();
        branch.pop_front();

        // apply action
        last_reward = env.act(action);
        if( node != nullptr ) node = node->advance(action);
        prefix.push_back(action);
        total_reward += last_reward;

        // prune branch if got positive reward
        if( execute_single_action || (last_reward > 0) )
            branch.clear();
    }
    return make_pair(total_reward, make_pair(decision, frame));
}

void parse_action_sequence(const string &action_sequence, vector<Action> &actions) {
    boost::tokenizer<> tok(action_sequence);
    for( boost::tokenizer<>::iterator it = tok.begin(); it != tok.end(); ++it )
        actions.push_back(static_cast<Action>(atoi(it->c_str())));
}

void usage(ostream &os, const po::options_description &opt_desc) {
    os << Utils::magenta() << "Usage:" << Utils::normal()
       << " rollout <option>* <rom>" << endl
       << endl
       << opt_desc
       << endl;
}

int main(int argc, char **argv) {
    // parameters
    bool use_minimal_action_set = false;
    int frameskip;
    int random_seed;
    bool display = true;
    bool sound = false;
    string rec_dir;
    string rec_sound_filename;
    int screen_features;
    bool feature_stratification = false;
    int max_depth;
    int max_rep;
    float discount;
    float alpha;
    bool debug = false;
    int num_episodes;
    int max_execution_length_in_frames;
    int num_frames_for_background_image;
    string action_sequence;
    bool execute_single_action = false;
    float budget_secs_per_decision;
    bool novelty_subtables = false;
    string log_file;
    string atari_rom;

    // declare supported options
    po::options_description opt_desc("Allowed options");
    opt_desc.add_options()
      ("help", "help message")
      ("minimal-action-set", "turn on minimal action set instead of larger legal action set")
      ("frameskip", po::value<int>(&frameskip)->default_value(5), "set frame skip rate")
      ("seed", po::value<int>(&random_seed)->default_value(0), "set random seed")
      ("nodisplay", "turn off display (default is display)")
      ("sound", "turn on sound (default is no sound)")
      ("rec-dir", po::value<string>(&rec_dir), "set folder for recording (default is \"\" for no recording)")
      ("rec-sound-filename", po::value<string>(&rec_sound_filename), "set filename for recording sound (default is \"\" for no recording)")
      ("features", po::value<int>(&screen_features)->default_value(0), "set feature set: 0=RAM, 1=basic, 2=basic+B-PROS, 3=basic+B-PROS+B-PROT (default is 0)")
      ("feature-stratification", "turn on feature stratification (default is off)")
      ("max-depth", po::value<int>(&max_depth)->default_value(1500), "set max depth for lookahead (default is 1500)")
      ("max-rep", po::value<int>(&max_rep), "set max rep(etition) of screen features during lookahead (default is 30 frames")
      ("discount", po::value<float>(&discount)->default_value(1.0), "set discount factor for lookahead (default is 1.0)")
      ("alpha", po::value<float>(&alpha)->default_value(10000.0), "set alpha value for lookahead (default is 10,000)")
      ("debug", "turn on debug (default is off)")
      ("num-episodes", po::value<int>(&num_episodes)->default_value(1), "set number of episodes (default is 1)")
      ("max-length", po::value<int>(&max_execution_length_in_frames)->default_value(18000), "set max number of frames in single execution (default is 18k frames)")
      ("frames-background-image", po::value<int>(&num_frames_for_background_image)->default_value(100), "set number of random frames to compute background image (default is 100 frames)")
      ("action-sequence", po::value<string>(&action_sequence), "pass fixed action sequence that provides actions (default is \"\" for no such sequence")
      ("budget-secs-per-decision", po::value<float>(&budget_secs_per_decision)->default_value(numeric_limits<float>::max()), "set budget time per decision in seconds (default is infinite)")
      ("novelty-subtables", "turn on use of novelty subtables (default is to use single table)")
      ("execute-single-action", "execute only one action from best branch in lookahead (default is to execute prefix until first reward")
      ("log-file", po::value<string>(&log_file), "set path to log file (default is \"\" for no logging)")
      ("rom", po::value<string>(&atari_rom), "set Atari ROM")
    ;

    po::positional_options_description opt_pos;
    opt_pos.add("rom", -1);

    // parse options
    po::variables_map opt_varmap;
    try {
        po::store(po::command_line_parser(argc, argv).options(opt_desc).positional(opt_pos).run(), opt_varmap);
        po::notify(opt_varmap);
    } catch( po::error &e ) {
        cout << Utils::error() << e.what() << endl;
        exit(1);
    }

    // set default values
    ostream *logos = &cout;;
    if( opt_varmap.count("use-minimal-action-set") )
        use_minimal_action_set = true;
    if( opt_varmap.count("nodisplay") )
        display = false;
    if( opt_varmap.count("sound") )
        sound = true;
    if( opt_varmap.count("feature-stratification") )
        feature_stratification = true;
    if( opt_varmap.count("debug") )
        debug = true;
    if( !opt_varmap.count("max-rep") )
        max_rep = 60 / frameskip;
    if( opt_varmap.count("execute-single-action") )
        execute_single_action = true;
    if( opt_varmap.count("novelty-subtables") )
        novelty_subtables = true;
    if( opt_varmap.count("log-file") ) {
        cout << "logging: file=" << log_file << endl;
        logos = new ofstream(log_file);
    }

    // check whether there is something to be done
    if( opt_varmap.count("help") || (atari_rom == "") ) {
        usage(cout, opt_desc);
        exit(1);
    }

    // print command-line options
    *logos << "options:" << endl;
    bool something_printed = false;
    for( po::variables_map::const_iterator it = opt_varmap.begin(); it != opt_varmap.end(); ++it ) {
        if( something_printed ) *logos << " \\" << endl;
        *logos << "  --" << it->first;
        if( ((boost::any)it->second.value()).type() == typeid(bool) ) {
            *logos << " " << opt_varmap[it->first].as<bool>();
        } else if( ((boost::any)it->second.value()).type() == typeid(int) ) {
            *logos << " " << opt_varmap[it->first].as<int>();
        } else if( ((boost::any)it->second.value()).type() == typeid(float) ) {
            *logos << " " << opt_varmap[it->first].as<float>();
        } else if( ((boost::any)it->second.value()).type() == typeid(string) ) {
            *logos << " " << opt_varmap[it->first].as<string>();
        }
        something_printed = true;
    }
    if( something_printed ) *logos << endl;

    // set random seed for lrand48()
    unsigned short seed[3];
    seed[0] = seed[1] = seed[2] = random_seed;
    seed48(seed);

    // create ALEs
    ALEInterface env, sim;

    // get/set desired settings
    env.setInt("frame_skip", frameskip);
    env.setInt("random_seed", random_seed);
    env.setFloat("repeat_action_probability", 0.00); // The default is already 0.25, this is just an example
    sim.setInt("frame_skip", frameskip);
    sim.setInt("random_seed", random_seed);
    sim.setFloat("repeat_action_probability", 0.00); // The default is already 0.25, this is just an example
    fs::path rom_path(atari_rom);

#ifdef __USE_SDL
    env.setBool("display_screen", display);
    env.setBool("sound", sound);
    if( rec_dir != "" ) {
        string full_rec_dir = rec_dir + "/" + rom_path.filename().string();
        env.setString("record_screen_dir", full_rec_dir.c_str());
        if( rec_sound_filename != "" )
            env.setString("record_sound_filename", (full_rec_dir + "/" + rec_sound_filename).c_str());
        fs::create_directories(full_rec_dir);
    }
#endif

    // Load the ROM file. (Also resets the system for new settings to take effect.)
    env.loadROM(rom_path.string().c_str());
    sim.loadROM(rom_path.string().c_str());

    // initialize static members for screen features
    if( screen_features > 0 ) {
        MyALEScreen::create_background_image();
        MyALEScreen::compute_background_image(sim, *logos, num_frames_for_background_image, true);
    }

    // construct planner
    Planner *planner = nullptr;
    if( action_sequence == "" ) {
        size_t num_tracked_atoms = 0;
        if( screen_features == 0 ) { // RAM mode
            num_tracked_atoms = 128 * 256; // this is for RAM: 128 8-bit entries
        } else {
            num_tracked_atoms = 16 * 14 * 128; // 28,672
            num_tracked_atoms += screen_features > 1 ? 6856768 : 0;
            num_tracked_atoms += screen_features > 2 ? 13713408 : 0;
        }
        planner = new RolloutIWPlanner(sim,
                                       *logos,
                                       use_minimal_action_set,
                                       frameskip,
                                       budget_secs_per_decision,
                                       novelty_subtables,
                                       screen_features,
                                       feature_stratification,
                                       num_tracked_atoms,
                                       max_depth,
                                       max_rep,
                                       discount,
                                       alpha,
                                       debug);
    } else {
        vector<Action> actions;
        parse_action_sequence(action_sequence, actions);
        planner = new FixedPlanner(actions);
    }
    assert(planner != nullptr);
    *logos << "planner=" << planner->name() << endl;

    // play
    for( size_t k = 0; k < num_episodes; ++k ) {
        vector<Action> prefix;
        float start_time = Utils::read_time_in_seconds();
        pair<float, pair<size_t, size_t> > p = run_trial(env, *logos, *planner, execute_single_action, frameskip, max_execution_length_in_frames, prefix);
        float elapsed_time = Utils::read_time_in_seconds() - start_time;
        *logos << "episode-stats:"
               << " score=" << p.first
               << ", decisions=" << p.second.first
               << ", frames=" << p.second.second
               << ", total-time=" << elapsed_time
               << ", avg-time-per-decision=" << elapsed_time / float(p.second.first)
               << ", avg-time-per-frame=" << elapsed_time / float(p.second.second)
               << endl;
    }

    // cleanup
    if( dynamic_cast<ofstream*>(logos) != nullptr ) {
        static_cast<ofstream*>(logos)->close();
        delete logos;
    }

    return 0;
}

