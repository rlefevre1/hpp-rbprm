// Copyright (c) 2014, LAAS-CNRS
// Authors: Steve Tonneau (steve.tonneau@laas.fr)
//
// This file is part of hpp-rbprm.
// hpp-rbprm is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-rbprm is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.  You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-rbprm. If not, see <http://www.gnu.org/licenses/>.

#include <hpp/rbprm/interpolation/rbprm-path-interpolation.hh>
#include <hpp/rbprm/contact_generation/algorithm.hh>
#include <hpp/model/configuration.hh>

#ifdef PROFILE
    #include "hpp/rbprm/rbprm-profiler.hh"
#endif

namespace hpp {
  namespace rbprm {
    namespace interpolation {

    RbPrmInterpolationPtr_t RbPrmInterpolation::create (const hpp::rbprm::RbPrmFullBodyPtr_t robot,
                                                        const hpp::rbprm::State &start, const hpp::rbprm::State &end,
                                                        const core::PathVectorConstPtr_t path)
    {
        RbPrmInterpolation* rbprmDevice = new RbPrmInterpolation(path, robot, start, end);
        RbPrmInterpolationPtr_t res (rbprmDevice);
        res->init (res);
        return res;
    }

    RbPrmInterpolation::~RbPrmInterpolation()
    {
        // NOTHING
    }

    // ========================================================================


    core::Configuration_t RbPrmInterpolation::configPosition(core::ConfigurationIn_t previous, const core::PathVectorConstPtr_t path, double i)
    {
        core::Configuration_t configuration = previous;
        size_t pathConfigSize = path->outputSize() - robot_->device_->extraConfigSpace().dimension();
        core::Configuration_t configPosition(path->outputSize());
        (*path)(configPosition,std::min(i, path->timeRange().second));
        configuration.head(pathConfigSize) = configPosition.head(pathConfigSize);
        configuration.tail(robot_->device_->extraConfigSpace().dimension()) = configPosition.tail(robot_->device_->extraConfigSpace().dimension()) ;
        configuration[2] = configuration[2] + 0.1; //walk static
        //configuration[2] = configuration[2] + 0.02; //stairs
        return configuration;
    }


    rbprm::T_StateFrame RbPrmInterpolation::Interpolate(const affMap_t& affordances,
            const std::map<std::string, std::vector<std::string> >& affFilters, const double timeStep, const double robustnessTreshold, const bool filterStates)
    {
        if(!path_) throw std::runtime_error ("Cannot interpolate; no path given to interpolator ");
        T_Configuration configs;
        const core::interval_t& range = path_->timeRange();
        configs.push_back(start_.configuration_);
		int j = 0;
        //for(double i = range.first + timeStep; i< range.second; i+= timeStep)
        for(double i = range.first; i< range.second; i+= timeStep, ++j)
        {
            configs.push_back(configPosition(configs.back(),path_,i));
        }
        configs.push_back(configPosition(configs.back(),path_,range.second));
        return Interpolate(affordances, affFilters, configs, robustnessTreshold, timeStep, range.first, filterStates);
    }

    rbprm::T_StateFrame RbPrmInterpolation::Interpolate(const affMap_t& affordances,
                                                        const std::map<std::string, std::vector<std::string> >& affFilters,
                                                        const hpp::rbprm::T_Configuration &configs, const double robustnessTreshold,
                                                        const model::value_type timeStep, const model::value_type initValue, const bool filterStates)
    {
        int nbFailures = 0;
        size_t accIndex = robot_->device_->configSize() - robot_->device_->extraConfigSpace().dimension () + 3 ; // index of the start of the acceleration vector (of size 3), in the configuration vector
        hppDout(notice,"acceleration index : "<<accIndex);
        model::value_type currentVal(initValue);
        rbprm::T_StateFrame states;
        states.push_back(std::make_pair(currentVal, this->start_));
        std::size_t nbRecontacts = 0;
        std::size_t repos = 0;
        bool allowFailure = true;
        Eigen::Vector3d dir,acc;
#ifdef PROFILE
    RbPrmProfiler& watch = getRbPrmProfiler();
    watch.reset_all();
    watch.start("complete generation");
#endif
        for(CIT_Configuration cit = configs.begin()+1; cit != configs.end(); ++cit, currentVal+= timeStep)
        {
            const State& previous = states.back().second;
            core::Configuration_t configuration = *cit;
            acc = configuration.segment<3>(accIndex);
            dir = configuration.head<3>() - previous.configuration_.head<3>();
            fcl::Vec3f direction(dir[0], dir[1], dir[2]);
            bool nonZero(false);
            direction.normalize(&nonZero);
            if(!nonZero) direction = fcl::Vec3f(0,0,1.);
            // TODO Direction 6d
            hpp::rbprm::contact::ContactReport rep = contact::ComputeContacts(previous, robot_,configuration, affordances,affFilters,direction,
                                             robustnessTreshold,acc);
            State& newState = rep.result_;


            const bool  sameAsPrevious = rep.success_ && rep.contactMaintained_;
            const bool& multipleBreaks = rep.multipleBreaks_;
            const bool& respositioned = rep.repositionedInPlace_;

            if(allowFailure && (!rep.success_ || rep.multipleBreaks_))
            {
                ++ nbFailures;
                if(cit != configs.end() && (cit+1)!= configs.end())
                {
                    ++cit;
                }
                currentVal+= timeStep;
if (nbFailures > 1)
{
    std::cout << "failed " << std::endl;
#ifdef PROFILE
    watch.stop("complete generation");
    watch.add_to_count("planner failed", 1);
    std::ofstream fout;
    fout.open("log.txt", std::fstream::out | std::fstream::app);
    std::ostream* fp = &fout;
    watch.report_count(*fp);
    fout.close();
#endif
    return FilterStates(states, filterStates);
    //return states;
}
            }
            if(multipleBreaks && !allowFailure)
            {
                ++nbRecontacts;
                cit--;
                currentVal-= timeStep;
            }
            else if(!multipleBreaks && respositioned)
            {
                ++nbRecontacts;
                cit--;
                currentVal-= timeStep;
            }
            else
            {
                nbRecontacts = 0;
            }
            if(respositioned)
            {
                ++repos;
                if (repos > 20)
                {
				std::cout<<"failed, too much repositionning"<<std::endl;
				#ifdef PROFILE
					watch.stop("complete generation");
					watch.add_to_count("planner failed", 1);
					std::ofstream fout;
					fout.open("log.txt", std::fstream::out | std::fstream::app);
					std::ostream* fp = &fout;
					watch.report_count(*fp);
					fout.close();
				#endif
                    return FilterStates(states, filterStates);
                }
            }
            if(sameAsPrevious && !multipleBreaks && !respositioned)
                states.pop_back();
            newState.nbContacts = newState.contactNormals_.size();
            states.push_back(std::make_pair(currentVal, newState));
            //allowFailure = nbRecontacts < robot_->GetLimbs().size();
            allowFailure = nbRecontacts < 2;
        }
        states.push_back(std::make_pair(this->path_->timeRange().second, this->end_));
#ifdef PROFILE
        watch.add_to_count("planner succeeded", 1);
        watch.stop("complete generation");
        /*std::ofstream fout;
        fout.open("log.txt", std::fstream::out | std::fstream::app);
        std::ostream* fp = &fout;
        watch.report_all_and_count(2,*fp);
        fout.close();*/
#endif
        return FilterStates(states, filterStates);
        //return states;
    }

    void RbPrmInterpolation::init(const RbPrmInterpolationWkPtr_t& weakPtr)
    {
        weakPtr_ = weakPtr;
    }

    RbPrmInterpolation::RbPrmInterpolation (const core::PathVectorConstPtr_t path, const hpp::rbprm::RbPrmFullBodyPtr_t robot, const hpp::rbprm::State &start, const hpp::rbprm::State &end)
        : path_(path)
        , start_(start)
        , end_(end)
        , robot_(robot)
    {
        // TODO
    }

    bool EqStringVec(const std::vector<std::string>& v1, const std::vector<std::string>& v2)
    {
        return (v1.size() == v2.size()) && std::equal ( v1.begin(), v1.end(), v2.begin() );
    }

    void FilterRepositioning(const CIT_StateFrame& from, const CIT_StateFrame to, T_StateFrame& res)
    {
        if(from == to) return;
        const State& current    = (from)->second;
        const State& current_m1 = (from-1)->second;
        const State& current_p1 = (from+1)->second;
        if(EqStringVec(current.contactBreaks(current_m1),
                       current_p1.contactBreaks(current_m1)) &&
           EqStringVec(current.contactCreations(current_m1),
                       current_p1.contactCreations(current)))
        {
            if(from+1 == to) return;
            res.push_back(std::make_pair((from+1)->first, (from+1)->second));
            FilterRepositioning(from+2, to, res);
        }
        else
        {
            res.push_back(std::make_pair(from->first, from->second));
            FilterRepositioning(from+1, to, res);
        }
    }

    void FilterBreakCreate(const CIT_StateFrame& from, const CIT_StateFrame to, T_StateFrame& res)
    {
        if(from == to) return;
        const State& current    = (from)->second;
        const State& current_m1 = (from-1)->second;
        const State& current_p1 = (from+1)->second;
        if(current.contactCreations(current_m1).empty()  &&
           current_p1.contactBreaks(current).empty() &&
           EqStringVec(current_p1.contactCreations(current),
                       current.contactBreaks(current_m1)))
        {
            if(from+1 == to) return;
            res.push_back(std::make_pair((from+1)->first, (from+1)->second));
            FilterBreakCreate(from+2, to, res);
        }
        else
        {
            res.push_back(std::make_pair(from->first, from->second));
            FilterBreakCreate(from+1, to, res);
        }
    }

    T_StateFrame FilterRepositioning(const T_StateFrame& originStates)
    {
        if(originStates.size() < 3) return originStates;
        T_StateFrame res;
        res.push_back(originStates.front());
        FilterRepositioning(originStates.begin()+1, originStates.end()-1, res);
        res.push_back(originStates.back());
        return res;
    }

    T_StateFrame FilterBreakCreate(const T_StateFrame& originStates)
    {
        if(originStates.size() < 3) return originStates;
        T_StateFrame res;
        res.push_back(originStates.front());
        FilterBreakCreate(originStates.begin()+1, originStates.end()-1, res);
        res.push_back(originStates.back());
        return res;
    }

    T_StateFrame FilterObsolete(const T_StateFrame& originStates)
    {
        if(originStates.size() < 3) return originStates;
        T_StateFrame res;
        res.push_back(originStates.front());
        CIT_StateFrame cit = originStates.begin();
        for(CIT_StateFrame cit2 = originStates.begin()+1;
            cit2 != originStates.end()-1; ++cit, ++cit2)
        {
            const State& current    = (cit2)->second;
            const State& current_m1 = (cit)->second;
            if((current.configuration_ - current_m1.configuration_).norm() > std::numeric_limits<double>::epsilon()
                    && !(current.contactBreaks(current_m1).empty() && current.contactCreations(current_m1).empty()))
            {
                res.push_back(std::make_pair(cit2->first, cit2->second));
            }
        }
        res.push_back(originStates.back());

        cit = res.begin();
        std::size_t idx = 0;
        for(T_StateFrame::const_iterator cit2 = res.begin()+1; cit2 != res.end()-1; ++cit, ++cit2, ++idx)
        {
            const State prev = cit->second;
            const State next = cit2->second;
            std::vector<std::string> breaks = next.contactBreaks(prev);
            std::vector<std::string> creations = next.contactCreations(prev);
            if(breaks.size() > 1 || creations.size() > 1)
            {
                std::cout << "AFTER FILTER " << std::endl;
                std::cout << "\t REMOVING CONTACT " << breaks.size() << std::endl;
                for(std::vector<std::string>::const_iterator tf = breaks.begin(); tf != breaks.end(); ++tf)
                    std::cout << "\t \t " << *tf << std::endl;
                std::cout << "\t CREATING CONTACT " << creations.size() << std::endl;
                for(std::vector<std::string>::const_iterator tf = creations.begin(); tf != creations.end(); ++tf)
                    std::cout << "\t \t " << *tf << std::endl;

                std::cout << "END AFTERAFTER FILTER  " << breaks.size() << std::endl;
            }

        }
        return res;
    }

    T_StateFrame FilterStatesRec(const T_StateFrame& originStates)
    {
        return FilterObsolete(FilterBreakCreate(FilterRepositioning(originStates)));
    }

    T_StateFrame FilterStates(const T_StateFrame& originStates, const bool deep)
    {
        //make sure they re ok
        T_StateFrame::const_iterator cit = originStates.begin();
        std::size_t idx = 0;
        for(T_StateFrame::const_iterator cit2 = originStates.begin()+1; cit2 != originStates.end()-1; ++cit, ++cit2, ++idx)
        {
            const State prev = cit->second;
            const State next = cit2->second;
            std::vector<std::string> breaks = next.contactBreaks(prev);
            std::vector<std::string> creations = next.contactCreations(prev);
            if(breaks.size() > 1 || creations.size() > 1)
            {
                std::cout << "BEFORE FILTER " << std::endl;
                std::cout << "\t REMOVING CONTACT " << breaks.size() << std::endl;
                for(std::vector<std::string>::const_iterator tf = breaks.begin(); tf != breaks.end(); ++tf)
                    std::cout << "\t \t " << *tf << std::endl;
                std::cout << "\t CREATING CONTACT " << creations.size() << std::endl;
                for(std::vector<std::string>::const_iterator tf = creations.begin(); tf != creations.end(); ++tf)
                    std::cout << "\t \t " << *tf<< std::endl;

                std::cout << "END BEFORE FILTER  " << breaks.size() << std::endl;
            }

        }

        T_StateFrame res = originStates;
        if(deep)
        {
            std::size_t previousSize;
            do
            {
                previousSize = res.size();
                res = FilterStatesRec(res);
            }
            while(res.size() != previousSize);
            return res;
        }
        else
        {
            return FilterObsolete(originStates);
        }
    }
    } // interpolation
  } // rbprm
} //hpp
