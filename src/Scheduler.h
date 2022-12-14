#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include "DRAM.h"
#include "Request.h"
#include "Controller.h"
#include <vector>
#include <map>
#include <list>
#include <functional>
#include <cassert>
#include <utility>

using namespace std;
using namespace std::placeholders;

namespace ramulator
{

template <typename T>
class Controller;

template <typename T>
class Scheduler
{
public:
    Controller<T>* ctrl;

    enum class Type {
        FCFS, FRFCFS, FRFCFS_Cap, FRFCFS_PriorHit, MAX
    } type = Type::FRFCFS_Cap;
    //} type = Type::FCFS;

    long cap = 16;

    Scheduler(Controller<T>* ctrl) : ctrl(ctrl) {}

    list<Request>::iterator get_head(list<Request>& q)
    {
      // TODO make the decision at compile time
      if (type != Type::FRFCFS_PriorHit) {
        if (!q.size())
            return q.end();

        auto head = q.begin();
        for (auto itr = next(q.begin(), 1); itr != q.end(); itr++)
            head = compare[int(type)](head, itr);

        return head;
      } else {
        if (!q.size())
            return q.end();

        auto head = q.begin();
        for (auto itr = next(q.begin(), 1); itr != q.end(); itr++) {
            head = compare[int(Type::FRFCFS_PriorHit)](head, itr);
        }

        if (this->ctrl->is_ready(head) && this->ctrl->is_row_hit(head)) {
          return head;
        }

        // prepare a list of hit request
        vector<vector<int>> hit_reqs;
        for (auto itr = q.begin() ; itr != q.end() ; ++itr) {
          if (this->ctrl->is_row_hit(itr)) {
            auto begin = itr->addr_vec.begin();
            // TODO Here it assumes all DRAM standards use PRE to close a row
            // It's better to make it more general.
            auto end = begin + int(ctrl->channel->spec->scope[int(T::Command::PRE)]) + 1;
            vector<int> rowgroup(begin, end); // bank or subarray
            hit_reqs.push_back(rowgroup);
          }
        }
        // if we can't find proper request, we need to return q.end(),
        // so that no command will be scheduled
        head = q.end();
        for (auto itr = q.begin(); itr != q.end(); itr++) {
          bool violate_hit = false;
          if ((!this->ctrl->is_row_hit(itr)) && this->ctrl->is_row_open(itr)) {
            // so the next instruction to be scheduled is PRE, might violate hit
            auto begin = itr->addr_vec.begin();
            // TODO Here it assumes all DRAM standards use PRE to close a row
            // It's better to make it more general.
            auto end = begin + int(ctrl->channel->spec->scope[int(T::Command::PRE)]) + 1;
            vector<int> rowgroup(begin, end); // bank or subarray
            for (const auto& hit_req_rowgroup : hit_reqs) {
              if (rowgroup == hit_req_rowgroup) {
                  violate_hit = true;
                  break;
              }
            }
          }
          if (violate_hit) {
            continue;
          }
          // If it comes here, that means it won't violate any hit request
          if (head == q.end()) {
            head = itr;
          } else {
            head = compare[int(Type::FRFCFS)](head, itr);
          }
        }

        return head;
      }
    }

private:
    typedef list<Request>::iterator ReqIter;
    function<ReqIter(ReqIter, ReqIter)> compare[int(Type::MAX)];
 };

template <typename T>
class RowPolicy
{
public:
    Controller<T>* ctrl;

    enum class Type {
        Closed, ClosedAP, Opened, Timeout, MAX
    } type = Type::Opened;

    int timeout = 50;

    RowPolicy(Controller<T>* _ctrl) : ctrl(_ctrl) {
      // Closed
      policy[0] = std::bind([] (typename T::Command cmd, RowPolicy *obj) -> vector<int> {
            for (auto& kv : obj->ctrl->rowtable->table) {
                if (!obj->ctrl->is_ready(cmd, kv.first))
                    continue;
                return kv.first;
            }
            return vector<int>();}, _1, this);

        // ClosedAP
      policy[1] = std::bind([] (typename T::Command cmd, RowPolicy *obj) -> vector<int> {
	  for (auto& kv : obj->ctrl->rowtable->table) {
	    if (!obj->ctrl->is_ready(cmd, kv.first))
	      continue;
	    return kv.first;
	  }
	  return vector<int>();}, _1, this);

        // Opened
      policy[2] = std::bind([] (typename T::Command cmd, RowPolicy *obj) -> vector<int> {
	  return vector<int>();}, _1, this); 

      // Timeout
      policy[3] = std::bind([] (typename T::Command cmd, RowPolicy *obj) -> vector<int> {
	  for (auto& kv : obj->ctrl->rowtable->table) {
	    auto& entry = kv.second;
	    if (obj->ctrl->clk - entry.timestamp < obj->timeout)
	      continue;
	    if (!obj->ctrl->is_ready(cmd, kv.first))
	      continue;
	    return kv.first;
	  }
	  return vector<int>();}, _1, this); 



    }

    vector<int> get_victim(typename T::Command cmd)
    {
        return policy[int(type)](cmd);
    }

private:
    function<vector<int>(typename T::Command)> policy[int(Type::MAX)];
};


template <typename T>
class RowTable
{
public:
    Controller<T>* ctrl;

    struct Entry {
        int row;
        int hits;
        long timestamp;
    };

    map<vector<int>, Entry> table;

    RowTable(Controller<T>* ctrl) : ctrl(ctrl) {}

    void update(typename T::Command cmd, const vector<int>& addr_vec, long clk)
    {
        auto begin = addr_vec.begin();
        auto end = begin + int(T::Level::Row);
        vector<int> rowgroup(begin, end); // bank or subarray
        int row = *end;

        T* spec = ctrl->channel->spec;

        if (spec->is_opening(cmd))
            table.insert({rowgroup, {row, 0, clk}});

        if (spec->is_accessing(cmd)) {
            // we are accessing a row -- update its entry
            auto match = table.find(rowgroup);
            assert(match != table.end());
            assert(match->second.row == row);
            match->second.hits++;
            match->second.timestamp = clk;
        } /* accessing */

        if (spec->is_closing(cmd)) {
          // we are closing one or more rows -- remove their entries
          int n_rm = 0;
          int scope;
          if (spec->is_accessing(cmd))
            scope = int(T::Level::Row) - 1; //special condition for RDA and WRA
          else
            scope = int(spec->scope[int(cmd)]);

          for (auto it = table.begin(); it != table.end();) {
            if (equal(begin, begin + scope + 1, it->first.begin())) {
              n_rm++;
              it = table.erase(it);
            }
            else
              it++;
          }

          assert(n_rm > 0);
        } /* closing */
    }

    int get_hits(const vector<int>& addr_vec, const bool to_opened_row = false)
    {
        auto begin = addr_vec.begin();
        auto end = begin + int(T::Level::Row);

        vector<int> rowgroup(begin, end);
        int row = *end;

        auto itr = table.find(rowgroup);
        if (itr == table.end())
            return 0;

        if(!to_opened_row && (itr->second.row != row))
            return 0;

        return itr->second.hits;
    }

    int get_open_row(const vector<int>& addr_vec) {
        auto begin = addr_vec.begin();
        auto end = begin + int(T::Level::Row);

        vector<int> rowgroup(begin, end);

        auto itr = table.find(rowgroup);
        if(itr == table.end())
            return -1;

        return itr->second.row;
    }
 };


} /*namespace ramulator*/

#endif /*__SCHEDULER_H*/
