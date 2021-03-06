#include <latticelm/pylm.h>
#include <latticelm/pylm-fst.h>
#include <latticelm/macros.h>
#include <latticelm/sampgen.h>
#include <fst/compose.h>

#include <boost/version.hpp>
#if BOOST_VERSION >= 105400
# include <boost/math/distributions/beta.hpp>
# include <boost/math/distributions/gamma.hpp>
#else
# include <boost/random/beta_distribution.hpp>
# include <boost/random/gamma_distribution.hpp>
#endif

using namespace latticelm;
using namespace fst;

#define PRIOR_DA 1.5
#define PRIOR_DB 1.5
#define PRIOR_SA 2.0
#define PRIOR_SB 1.0
#define DEFAULT_DISC 0.5
#define DEFAULT_STREN 1.0

#define EPS_ID 0
#define PHI_ID 1
#define BOS_ID 2
#define EOS_ID 3
#define FALLBACK_ID 1

PylmStateLink * Pylm::GetChildStateLink(int sid, WordId wid, bool add) {
  assert(wid >= 0);
  assert(sid < (int)states_.size() && sid >= 0);
  vector<PylmStateLink> & chil = states_[sid].children;
  int low = 0, range = chil.size(), mid;
  while(range) {
    mid = low+range/2;
    if(chil[mid].wid > wid) {
      range = mid-low;
    } else if(chil[mid].wid < wid) {
      range -= mid-low+1;
      low = mid+1;
    } else {
      return &chil[mid];
    }
  }
  if(add && order_ > 1) {
    int backoff_sid = (states_[sid].backoff_sid == -1 ? 0 : GetChildStateId(states_[sid].backoff_sid, wid, add));
    if(states_[sid].level == order_-1) {
      chil.insert(chil.begin()+low, PylmStateLink(backoff_sid, wid, 0));
    } else {
      chil.insert(chil.begin()+low, PylmStateLink(states_.size(), wid, 0));
      states_.push_back(PylmState(wid, states_[sid].level+1, backoff_sid));
    }
    return &states_[sid].children[low];
  }
  return NULL; 
}

std::vector<PylmWordProbState> Pylm::CalcWordProbStates(int sid) {
  PylmState & state = states_[sid];
  std::vector<PylmWordProbState> wps;
  if(state.wid == EOS_ID) return wps; // If we're at a final state, we don't do anything
  assert(state.level < (int)params_.size());
  pair<float,float> & param = params_[state.level];
  // If we're at the top state, calculate everything, but no backoff
  if(state.level == 0 && base_size_ > 0) {
    float base_prob = 1.f/base_size_;
    auto it = state.children.begin();
    for(int i = 1; i <= base_size_; i++) {
      if(it != state.children.end() && it->wid == i) {
        wps.push_back(state.CalcWPS(*it++, param));
        wps.rbegin()->prob += base_prob;
      } else {
        wps.push_back(PylmWordProbState(i, base_prob, 0));
      }
    }
  // Otherwise, calculate a backoff and only some things
  } else {
    wps.push_back(PylmWordProbState(FALLBACK_ID,state.CalcBackoff(param),state.backoff_sid));
    for(auto & itval : state.children)
      if(itval.total_customers != 0)
        wps.push_back(state.CalcWPS(itval, param));
  }
  return wps;
}

inline void AddToVec(int val, std::vector<int> & vec) {
  if((int)vec.size() <= val) vec.resize(val+1, 0);
  vec[val]++;
}

void Pylm::ResampleParameters() {
  std::uniform_real_distribution<float> ud(0.f, 1.f);
  // auxiliary variables method
  for(int i = order_-1; i >= 0; i--) {
    float stren = params_[i].first, disc = params_[i].second;
    // Gather the counts for all states of this order
    vector<int> state_table_counts, state_cust_counts, table_cust_counts;
    for(auto & state : states_) {
      if(state.level != i) continue;
      AddToVec(state.total_tables, state_table_counts);
      AddToVec(state.total_customers, state_cust_counts);
      for(auto & child : state.children)
        for(int cust : child.customers)
          AddToVec(cust, table_cust_counts);
    }
    // Re-sample the parameters
    float da = PRIOR_DA, db = PRIOR_DB, sa = PRIOR_SA, sb = PRIOR_SB;
    for(int cnt = 1; cnt < (int)state_table_counts.size(); cnt++) {
      for(int j = 1; j < cnt; j++) {
        std::bernoulli_distribution bd(stren/(stren+disc*j));
        for(int k = 0; k < state_table_counts[cnt]; k++) {
          if(bd(*GlobalVars::rndeng)) { sa++; } else { da++; }
        }
      }
    }
    for(int cnt = 2; cnt < (int)state_cust_counts.size(); cnt++) {
#if BOOST_VERSION >= 105400
      boost::math::beta_distribution<float> bd(stren+1, cnt-1);
      for(int k = 0; k < state_cust_counts[cnt]; k++)
        sb -= log(quantile(bd, ud(*GlobalVars::rndeng)));
#else
      boost::random::beta_distribution<float> bd(stren+1, cnt-1);
      for(int k = 0; k < state_cust_counts[cnt]; k++)
        sb -= log(bd(*GlobalVars::rndeng));
#endif
    }
    for(int cnt = 1; cnt < (int)table_cust_counts.size(); cnt++) {
      for(int j = 1; j < cnt; j++) {
        std::bernoulli_distribution bd(1 - (j-1)/(j-disc));
        for(int k = 0; k < table_cust_counts[cnt]; k++)
          db += bd(*GlobalVars::rndeng);
      }
    }
#if BOOST_VERSION >= 105400
    boost::math::gamma_distribution<float> gd(sa,1/sb);
    params_[i].first = quantile(gd, ud(*GlobalVars::rndeng)); // gammaSample(sa,1/sb);
    boost::math::beta_distribution<float> bd(da,db);
    params_[i].second = quantile(bd, ud(*GlobalVars::rndeng)); // betaSample(da,db);
#else
    boost::random::gamma_distribution<float> gd(sa,1/sb);
    params_[i].first = gd(*GlobalVars::rndeng); // gammaSample(sa,1/sb);
    boost::random::beta_distribution<float> bd(da,db);
    params_[i].second = bd(*GlobalVars::rndeng); // betaSample(da,db);
#endif
    cerr << "  params_[" << i << "] == " << params_[i] << endl;
  }
}

bool Pylm::RemoveNgram(const Sentence & ngram) {
  int j, state = 0;
  for(j = 0; j < (int)ngram.size()-1; j++) {
    state = GetChildStateId(state, ngram[j], false);
    assert(state != -1);
  }
  PylmStateLink* link = GetChildStateLink(state, ngram[j], false);
  assert(link != NULL);
  assert(link->total_customers != 0);
  std::uniform_int_distribution<int> ud(1, link->total_customers);
  int left = ud(*GlobalVars::rndeng);
  link->total_customers--;
  assert(link->total_customers >= 0);
  states_[state].total_customers--;
  assert(states_[state].total_customers >= 0);
  for(int t = 0; t < (int)link->customers.size(); t++) {
    left -= link->customers[t];
    if(left <= 0) {
      if(link->customers[t] == 1) {
        states_[state].total_tables--;
        assert(states_[state].total_tables >= 0);
        link->customers.erase(link->customers.begin()+t);
        if(ngram.size() == 1)
          return true;
        Sentence new_ngram(ngram);
        new_ngram.erase(new_ngram.begin());
        return RemoveNgram(new_ngram);
      } else {
        link->customers[t]--;
        assert(link->customers[t] >= 0);
        return false;
      }
    }
  }
  THROW_ERROR("Overflowed RemoveNgram");
}

void Pylm::RemoveSample(const Sentence & sent, vector<bool> & fellback) {
  Sentence ngram(1,1); // Starting with just the wordId corresponding to <s>
  for(size_t i = 0; i < sent.size(); i++) {
    if((int)ngram.size() >= order_) ngram.erase(ngram.begin());
    ngram.push_back(sent[i]);
    if(fellback.size()) {
      fellback[i] = RemoveNgram(ngram);
    } else {
      RemoveNgram(ngram);
    }
  }
}

bool Pylm::AddNgram(const Sentence & ngram, float base) {
  int j, t, lev, state; // i=first word, j=curr word, t=table, lev=context length
  vector<float> full_probs(1,base), single_probs;
  vector<int> states;
  // Calculate the probabilities from unigram onward
  for(lev = 0; lev < (int)ngram.size(); lev++) {
    for(j = (int)ngram.size()-lev-1, state = 0; j < (int)ngram.size()-1; j++)
      state = GetChildStateId(state, ngram[j], true);
    PylmStateLink* link = GetChildStateLink(state, ngram[j], true);
    assert(lev >= 0 && lev < (int)params_.size());
    float backoff = states_[state].CalcBackoff(params_[lev]);
    float prob = states_[state].CalcProb(*link,params_[lev]);
    single_probs.push_back(prob);
    full_probs.push_back((*full_probs.rbegin())*backoff + prob);
    states.push_back(state);
  }
  // Get the links one more time, because pointers might have changed.
  vector<PylmStateLink*> links;
  for(lev = 0; lev < (int)ngram.size(); lev++) {
    for(j = (int)ngram.size()-lev-1, state = 0; j < (int)ngram.size()-1; j++)
      state = GetChildStateId(state, ngram[j], true);
    links.push_back(GetChildStateLink(state, ngram[j], true));
  }
  // Next, going backwards, at each level decide whether to add a new table
  for(lev = ngram.size()-1; lev >= 0; lev--) {
    std::bernoulli_distribution bd(single_probs[lev]/full_probs[lev+1]);
    states_[states[lev]].total_customers++;
    // Use existing table
    if(bd(*GlobalVars::rndeng)) {
      std::uniform_real_distribution<float> ud(0, links[lev]->total_customers - links[lev]->customers.size() * params_[lev].second);
      float left = ud(*GlobalVars::rndeng);
      for(t = 0; t < (int)links[lev]->customers.size(); t++) {
        left -= links[lev]->customers[t]-params_[lev].second;
        if(left <= 0) break;
      }
      assert(t < (int)links[lev]->customers.size());
      links[lev]->customers[t]++; links[lev]->total_customers++;
      break;
    // Use new table
    } else {
      links[lev]->customers.push_back(1); links[lev]->total_customers++; 
      states_[states[lev]].total_tables++;
    }
  }
  return lev < 0;
}

void Pylm::AddSample(const Sentence & sent, const vector<float> & bases, vector<bool> & fellback) {
  assert(base_size_ != 0 || (bases.size() == fellback.size() && bases.size() == sent.size()));
  Sentence ngram(1,1);
  for(size_t i = 0; i < sent.size(); i++) {
    if((int)ngram.size() >= order_) ngram.erase(ngram.begin());
    ngram.push_back(sent[i]);
    if(bases.size()) {
      fellback[i] = AddNgram(ngram, bases[i]);
    } else {
      AddNgram(ngram, 1.f/base_size_);
    }
  }
}

bool Pylm::CheckEmpty() {
  bool ret = true;
  for(size_t i = 0; i < states_.size(); i++) {
    auto & state = states_[i];
    if(state.total_customers != 0 || state.total_tables != 0) {
      cerr << "state " << i << " not empty: cust=" << state.total_customers << ", tables=" << state.total_tables << endl;
      ret = false;
    }
    for(auto & child : state.children) {
      if(child.total_customers != 0 || child.customers.size() != 0) {
        cerr << "child " << i << "->" << child.wid << " not empty: cust=" << child.total_customers << ", tables=" << child.customers.size() << endl;
        ret = false;
      }
    }
  }
  return ret;
}

Sentence Pylm::CreateSample(const DataLattice & lattice, LLStats & stats) {
  PylmFst<LogArc> pylm_fst(*this);
  
  // Phi transitions
  typedef PhiMatcher< Matcher<Fst<LogArc> > > PM;
  ComposeFstOptions<LogArc, PM> copts(CacheOptions(),
                        new PM(lattice.GetFst(), MATCH_NONE),
                        new PM(pylm_fst, MATCH_INPUT, PHI_ID));
  ComposeFst<LogArc> composed_fst(lattice.GetFst(), pylm_fst, copts);

  // // Epsilon transitions
  // ComposeFst<LogArc> composed_fst(lattice.GetFst(), pylm_fst);

  VectorFst<LogArc> sample_fst;
  stats.lik_ += SampGen(composed_fst, sample_fst);
  Sentence sent = FstToSent(sample_fst);
  stats.words_ += sent.size();
  return sent;
}
