//
// Copyright (c) 2017 CNRS
// Authors: Steve Tonneau
//
// This file is part of hpp-rbprm
// hpp-core is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-core is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.  You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-core  If not, see
// <http://www.gnu.org/licenses/>.

#include <hpp/rbprm/contact_generation/contact_generation.hh>
#include <hpp/rbprm/stability/stability.hh>
#include <hpp/rbprm/tools.hh>



namespace hpp {
namespace rbprm {
namespace contact{


ContactGenHelper::ContactGenHelper(RbPrmFullBodyPtr_t fb, const State& ps, model::ConfigurationIn_t configuration,
                                    const hpp::rbprm::affMap_t &affordances, const std::map<std::string, std::vector<std::string> > &affFilters,
                                    const double robustnessTreshold,
                                    const std::size_t maxContactBreaks, const std::size_t maxContactCreations,
                                    const bool checkStabilityMaintain, const bool checkStabilityGenerate,
                                    const fcl::Vec3f& direction,
                                    const fcl::Vec3f& acceleration,
                                    const bool contactIfFails,
                                    const bool stableForOneContact)
: fullBody_(fb)
, previousState_(ps)
, checkStabilityMaintain_(checkStabilityMaintain)
, contactIfFails_(contactIfFails)
, stableForOneContact_(stableForOneContact)
, acceleration_(acceleration)
, direction_(direction)
, robustnessTreshold_(robustnessTreshold)
, maxContactBreaks_(maxContactBreaks)
, maxContactCreations_(maxContactCreations)
, affordances_(affordances)
, affFilters_(affFilters)
, workingState_(previousState_)
, checkStabilityGenerate_(checkStabilityGenerate)
{
    workingState_.configuration_ = configuration;
    workingState_.stable = false;
}

typedef std::vector<T_State > T_DepthState;

bool push_if_new(T_State& states, const State currentState)
{
    for(CIT_State cit = states.begin(); cit != states.end(); ++cit)
    {
        if(currentState.contactOrder_== cit->contactOrder_)
            return false;
    }
    states.push_back(currentState);
    return true;
}

void maintain_contacts_combinatorial_rec(const hpp::rbprm::State& currentState, const std::size_t  depth,
                                         const std::size_t maxBrokenContacts, T_DepthState& res)
{
    if (!push_if_new(res[depth], currentState) || depth>=maxBrokenContacts) return;
    std::queue<std::string> contactOrder = currentState.contactOrder_;
    while(!contactOrder.empty())
    {
        hpp::rbprm::State copyState = currentState;
        const std::string contactRemoved = contactOrder.front();
        copyState.RemoveContact(contactRemoved);
        contactOrder.pop();
        maintain_contacts_combinatorial_rec(copyState, depth+1, maxBrokenContacts, res);
    }
}

Q_State flatten(const T_DepthState& depthstates)
{
    Q_State res;
    for(T_DepthState::const_iterator cit = depthstates.begin(); cit != depthstates.end(); ++cit)
    {
        for(CIT_State ccit = cit->begin(); ccit != cit->end(); ++ccit)
            res.push(*ccit);
    }
    return res;
}

Q_State maintain_contacts_combinatorial(const hpp::rbprm::State& currentState, const std::size_t maxBrokenContacts)
{
    T_DepthState res(maxBrokenContacts+1);
    maintain_contacts_combinatorial_rec(currentState, 0, maxBrokenContacts,res);
    return flatten(res);
}

using namespace projection;

bool maintain_contacts_stability_rec(hpp::rbprm::RbPrmFullBodyPtr_t fullBody,
                        model::ConfigurationIn_t targetRootConfiguration,
                        Q_State& candidates,const std::size_t contactLength,
                        const fcl::Vec3f& acceleration, const double robustness,
                        ProjectionReport& currentRep)
{
    if(stability::IsStable(fullBody,currentRep.result_) > robustness)
    {
        currentRep.result_.stable = true;
        return true;
    }
    currentRep.result_.stable = false;
    if(!candidates.empty())
    {
        State cState = candidates.front();
        candidates.pop();
         // removed more contacts, cannot be stable if previous state was not
        if(cState.contactOrder_.size() < contactLength) return false;
        ProjectionReport rep = projectToRootConfiguration(fullBody,targetRootConfiguration,cState);
        Q_State copy_candidates = candidates;
        if(maintain_contacts_stability_rec(fullBody,targetRootConfiguration,copy_candidates,contactLength,acceleration, robustness, rep))
        {
            currentRep = rep;
            candidates = copy_candidates;
            return true;
        }
    }
    return false;
}


hpp::model::ObjectVector_t getAffObjectsForLimb(const std::string& limb,
    const affMap_t& affordances, const std::map<std::string, std::vector<std::string> >& affFilters)
{
    model::ObjectVector_t affs;
    std::vector<std::string> affTypes;
    bool settingFound = false;
    for (std::map<std::string, std::vector<std::string> >::const_iterator fIt =
        affFilters.begin (); fIt != affFilters.end (); ++fIt)
    {
        std::size_t found = fIt->first.find(limb);
        if (found != std::string::npos)
        {
            affTypes = fIt->second;
            settingFound = true;
            break;
        }
    }
    if (!settingFound)
    {
        // TODO: Keep warning or delete it?
        std::cout << "No affordance filter setting found for limb " << limb
            << ". Has such filter been set?" << std::endl;
        // Use all AFF OBJECTS as default if no filter setting exists
        for (affMap_t::const_iterator affordanceIt = affordances.begin ();
            affordanceIt != affordances.end (); ++affordanceIt)
        {
            std::copy (affordanceIt->second.begin (), affordanceIt->second.end (), std::back_inserter (affs));
        }
    }
    else
    {
        for (std::vector<std::string>::const_iterator affTypeIt = affTypes.begin ();
            affTypeIt != affTypes.end (); ++affTypeIt)
        {
            affMap_t::const_iterator affIt = affordances.find(*affTypeIt);
            std::copy (affIt->second.begin (), affIt->second.end (), std::back_inserter (affs));
        }
    }
    if (affs.empty())
        throw std::runtime_error ("No aff objects found for limb " + limb);
    return affs;
}

ProjectionReport maintain_contacts_stability(ContactGenHelper &contactGenHelper, ProjectionReport& currentRep)
{
    const std::size_t contactLength(currentRep.result_.contactOrder_.size());
//contactGenHelper.candidates_.pop(); // TODO REMOVE (TEST)
    maintain_contacts_stability_rec(contactGenHelper.fullBody_,
                                    contactGenHelper.workingState_.configuration_,
                                    contactGenHelper.candidates_,
                                    contactLength, contactGenHelper.acceleration_,
                                    contactGenHelper.robustnessTreshold_, currentRep);
    return currentRep;
}


std::vector<std::string> extractEffectorsName(const rbprm::T_Limb& limbs)
{
    std::vector<std::string> res;
    for(rbprm::T_Limb::const_iterator cit = limbs.begin(); cit != limbs.end(); ++cit)
        res.push_back(cit->first);
    return res;
}

ProjectionReport genColFree(ContactGenHelper &contactGenHelper, ProjectionReport& currentRep)
{
    ProjectionReport res = currentRep;
    // identify broken limbs and find collision free configurations for each one of them.
    std::vector<std::string> effNames(extractEffectorsName(contactGenHelper.fullBody_->GetLimbs()));
    const std::vector<std::string> freeLimbs = rbprm::freeEffectors(currentRep.result_,effNames.begin(), effNames.end() );
    for(std::vector<std::string>::const_iterator cit = freeLimbs.begin(); cit != freeLimbs.end() && res.success_; ++cit)
        res = projection::setCollisionFree(contactGenHelper.fullBody_,contactGenHelper.fullBody_->GetLimbCollisionValidation().at(*cit),*cit,res.result_);

    return res;
}

void stringCombinatorialRec(std::vector<std::vector<std::string> >& res, const std::vector<std::string>& candidates, const std::size_t depth)
{
    if(depth == 0) return;
    std::vector<std::vector<std::string> > newStates;
    for(std::vector<std::vector<std::string> >::iterator it = res.begin(); it != res.end(); ++it)
    {
        for(std::vector<std::string>::const_iterator canditates_it = candidates.begin(); canditates_it != candidates.end(); ++canditates_it)
        {
            std::vector<std::string> contacts = *it;
            if(tools::insertIfNew(contacts,*canditates_it))
            {
                newStates.push_back(contacts);
            }
        }
    }
    stringCombinatorialRec(newStates, candidates, depth-1);
    res.insert(res.end(),newStates.begin(),newStates.end());
}


std::vector<std::vector<std::string> > stringCombinatorial(const std::vector<std::string>& candidates, const std::size_t maxDepth)
{
    std::vector<std::vector<std::string> > res;
    std::vector<std::string> tmp;
    res.push_back(tmp);
    stringCombinatorialRec(res, candidates, maxDepth);
    return res;
}

void gen_contacts_combinatorial_rec(const std::vector<std::string>& freeEffectors, const State& previous, T_ContactState& res, const std::size_t maxCreatedContacts)
{
    std::vector<std::vector<std::string> > allNewStates = stringCombinatorial(freeEffectors, maxCreatedContacts);
    for(std::vector<std::vector<std::string> >::const_iterator cit = allNewStates.begin(); cit!=allNewStates.end();++cit)
    {
        ContactState contactState; contactState.first = previous; contactState.second = *cit;
        res.push(contactState);
    }
}

T_ContactState gen_contacts_combinatorial(const std::vector<std::string>& freeEffectors, const State& previous, const std::size_t maxCreatedContacts)
{
    T_ContactState res;;
    gen_contacts_combinatorial_rec(freeEffectors, previous, res, maxCreatedContacts);
    return res;
}

T_ContactState gen_contacts_combinatorial(ContactGenHelper& contactGenHelper)
{
    State& cState = contactGenHelper.workingState_;
    std::vector<std::string> effNames(extractEffectorsName(contactGenHelper.fullBody_->GetLimbs()));
    const std::vector<std::string> freeLimbs = rbprm::freeEffectors(cState,effNames.begin(), effNames.end() );
    return gen_contacts_combinatorial(freeLimbs, cState, contactGenHelper.maxContactCreations_);
}


ProjectionReport maintain_contacts(ContactGenHelper &contactGenHelper)
{
    ProjectionReport rep;
    Q_State& candidates = contactGenHelper.candidates_;
    if(candidates.empty())
        candidates = maintain_contacts_combinatorial(contactGenHelper.workingState_,contactGenHelper.maxContactBreaks_);
    else
        candidates.pop(); // first candidate already treated.
    while(!candidates.empty() && !rep.success_)
    {
        //retrieve latest state
        State cState = candidates.front();
        candidates.pop();
        rep = projectToRootConfiguration(contactGenHelper.fullBody_,contactGenHelper.workingState_.configuration_,cState);
        if(rep.success_)
            rep = genColFree(contactGenHelper, rep);
        if(rep.success_)
        {
            //collision validation
            hpp::core::ValidationReportPtr_t valRep (new hpp::core::CollisionValidationReport);
            rep.success_ = contactGenHelper.fullBody_->GetCollisionValidation()->validate(rep.result_.configuration_, valRep);
        }
    }
    if(rep.success_ && contactGenHelper.checkStabilityMaintain_)
        return maintain_contacts_stability(contactGenHelper, rep);
    return rep;
}


sampling::T_OctreeReport CollideOctree(const ContactGenHelper &contactGenHelper, const std::string& limbName,
                                                    RbPrmLimbPtr_t limb, const sampling::heuristic evaluate)
{
    fcl::Transform3f transform = limb->octreeRoot(); // get root transform from configuration
    hpp::model::ObjectVector_t affordances = getAffObjectsForLimb (limbName,contactGenHelper.affordances_, contactGenHelper.affFilters_);

    //#pragma omp parallel for
    // request samples which collide with each of the collision objects
    sampling::heuristic eval =  evaluate == 0 ? limb->evaluate_ : evaluate;
    std::size_t i (0);
    if (affordances.empty ())
      throw std::runtime_error ("No aff objects found!!!");

    std::vector<sampling::T_OctreeReport> reports(affordances.size());
    for(model::ObjectVector_t::const_iterator oit = affordances.begin();
        oit != affordances.end(); ++oit, ++i)
    {
        if(eval)
            sampling::GetCandidates(limb->sampleContainer_, transform, *oit, contactGenHelper.direction_, reports[i], eval);
        else
            sampling::GetCandidates(limb->sampleContainer_, transform, *oit, contactGenHelper.direction_, reports[i]);
    }
    sampling::T_OctreeReport finalSet;
    // order samples according to EFORT
    for(std::vector<sampling::T_OctreeReport>::const_iterator cit = reports.begin();
        cit != reports.end(); ++cit)
    {
        finalSet.insert(cit->begin(), cit->end());
    }
    return finalSet;
}

hpp::rbprm::State findValidCandidate(const ContactGenHelper &contactGenHelper, const std::string& limbId,
                        RbPrmLimbPtr_t limb, core::CollisionValidationPtr_t validation, bool& found_sample,
                                     bool& unstableContact, const sampling::heuristic evaluate = 0)
{
    State current = contactGenHelper.workingState_;
    current.stable = false;
    sampling::T_OctreeReport finalSet = CollideOctree(contactGenHelper, limbId, limb, evaluate);
    core::Configuration_t moreRobust, configuration;
    configuration = current.configuration_;
    double maxRob = -std::numeric_limits<double>::max();
    sampling::T_OctreeReport::const_iterator it = finalSet.begin();
    fcl::Vec3f position, normal;
    fcl::Matrix3f rotation;
    ProjectionReport rep ;
    for(;!found_sample && it!=finalSet.end(); ++it)
    {
        const sampling::OctreeReport& bestReport = *it;
        /*ProjectionReport */rep = projectSampleToObstacle(contactGenHelper.fullBody_, limbId, limb, bestReport, validation, configuration, current);
        if(rep.success_)
        {
            double robustness = stability::IsStable(contactGenHelper.fullBody_,rep.result_);
            if(    !contactGenHelper.checkStabilityGenerate_
                || (rep.result_.nbContacts == 1 && !contactGenHelper.stableForOneContact_)
                || robustness>=contactGenHelper.robustnessTreshold_)
            {
                maxRob = std::max(robustness, maxRob);
                position = limb->effector_->currentTransformation().getTranslation();
                rotation = limb->effector_->currentTransformation().getRotation();
                normal = rep.result_.contactNormals_.at(limbId);
                found_sample = true;
            }
            // if no stable candidate is found, select best contact
            // anyway
            else if((robustness > maxRob) && contactGenHelper.contactIfFails_)
            {
                moreRobust = configuration;
                maxRob = robustness;
                position = limb->effector_->currentTransformation().getTranslation();
                rotation = limb->effector_->currentTransformation().getRotation();
                normal = rep.result_.contactNormals_.at(limbId);
                unstableContact = true;
            }
        }
    }
    if(found_sample || unstableContact)
    {
        current.contacts_[limbId] = true;
        current.contactNormals_[limbId] = normal;
        current.contactPositions_[limbId] = position;
        current.contactRotation_[limbId] = rotation;
        current.contactOrder_.push(limbId);
    }
    if(found_sample)
    {
        current.configuration_ = configuration;
        current.stable = true;
    }
    if(unstableContact)
    {
        current.configuration_ = moreRobust;
        current.stable = false;
    }
    return current;
}

ProjectionReport generate_contact(const ContactGenHelper &contactGenHelper, const std::string& limbName,
                                  const sampling::heuristic evaluate)
{
    ProjectionReport rep;

    RbPrmLimbPtr_t limb = contactGenHelper.fullBody_->GetLimbs().at(limbName);
    core::CollisionValidationPtr_t validation = contactGenHelper.fullBody_->GetLimbCollisionValidation().at(limbName);
    limb->limb_->robot()->currentConfiguration(contactGenHelper.workingState_.configuration_);
    limb->limb_->robot()->computeForwardKinematics ();

    // pick first sample which is collision free
    bool found_sample(false);
    bool unstableContact(false); //set to true in case no stable contact is found
    rep.result_ = findValidCandidate(contactGenHelper,limbName,limb, validation, found_sample,unstableContact, evaluate);
    if(found_sample)
    {
        rep.status_ = STABLE_CONTACT;
        rep.success_ = true;
#ifdef PROFILE
  RbPrmProfiler& watch = getRbPrmProfiler();
  watch.add_to_count("contact", 1);
#endif
    }
    else if(unstableContact)
    {
        rep.status_ = UNSTABLE_CONTACT;
        rep.success_ = !contactGenHelper.checkStabilityGenerate_;
#ifdef PROFILE
  RbPrmProfiler& watch = getRbPrmProfiler();
  watch.add_to_count("unstable contact", 1);
#endif
    }
    else
    {
        rep =  setCollisionFree(contactGenHelper.fullBody_,validation,limbName,rep.result_);
        rep.status_ = NO_CONTACT;
        rep.success_ = false;
#ifdef PROFILE
  RbPrmProfiler& watch = getRbPrmProfiler();
  watch.add_to_count("no contact", 1);
#endif
    }
    return rep;
}

ProjectionReport gen_contacts(ContactGenHelper &contactGenHelper)
{
    ProjectionReport rep;
    T_ContactState candidates = gen_contacts_combinatorial(contactGenHelper);
    //remove candidates which not respect the required limbs in contact
    T_ContactState candidates_tmp;
    while(!candidates.empty())
    {
        /*
            * When the bug of the contacts map will be solved, remove the mode 0 and keep only the good one (enhanced version)
            * The mode 0 is not optimal in terms of complexity but allows us to avoid a crash (due to the inexistence of a required contact in the map)
            * The enhanced mode throws an out-of-range exception when we have required contacts because of the contacts map bug (some contacts disappeared unexpectedly in the map)
        */
        int mode(0); // contact checking mode : mode == 0 --> normal, mode != 0 --> enhanced version
        if(mode == 0)
        {
            // get the actives contacts in the state in the current ContactState
            std::vector <std::string> activesContacts;
            ContactState current = candidates.front();
            for(std::map<std::string, bool>::const_iterator cit = current.first.contacts_.begin(); cit != current.first.contacts_.end(); ++cit)
            {
                if(cit->second == true)
                {
                    activesContacts.push_back(cit->first);
                }
            }
            // check if all the required limbs appeared in the actives contacts set
            bool reqLimValid(true);
            std::vector <std::string> reqLimbs(contactGenHelper.fullBody_->getRequiredLimbs());
            for(unsigned int i = 0 ; reqLimValid && (i < reqLimbs.size()) ; ++i)
            {
                bool found(false);
                for(unsigned int j = 0 ; !found && (j < activesContacts.size()) ; ++j)
                {
                    if(reqLimbs[i] == activesContacts[j])
                        found = true;
                }
                if(!found)
                    reqLimValid = false;
            }
            // if yes, keep this ContactState
            if(reqLimValid)
            {
                candidates_tmp.push(current);
            }
            // remove the current ContactState of the queue
            candidates.pop();
        }
        else
        {
            //Enhanced version for the required contacts checking
            ContactState current = candidates.front();
            std::vector <std::string> reqLimbs(contactGenHelper.fullBody_->getRequiredLimbs());
            bool reqLimValid(true);
            // For each required limbs, checks if its active value in the contacts map is not to false
            std::cout << "nbContacts : " << current.first.contacts_.size() << std::endl;
            for(unsigned int i = 0 ; reqLimValid && (i < reqLimbs.size()) ; ++i)
            {
                //std::cout << "Iteration : " << i << " --- " << "Limb : " << reqLimbs[i] << std::endl;
                if(current.first.contacts_.at(reqLimbs[i]) == false)
                    reqLimValid = false;
            }
            if(reqLimValid)
            {
                candidates_tmp.push(current);
            }
            candidates.pop();
        }
    }
    candidates = candidates_tmp;
    while(!candidates.empty() && !rep.success_)
    {
        //retrieve latest state
        ContactState cState = candidates.front();
        candidates.pop();
        bool checkStability(contactGenHelper.checkStabilityGenerate_);
        //contactGenHelper.checkStabilityGenerate_ = false; // stability not mandatory before last contact is created
        if(cState.second.empty() && (contactGenHelper.workingState_.stable || (stability::IsStable(contactGenHelper.fullBody_,contactGenHelper.workingState_) > contactGenHelper.robustnessTreshold_ )) )
        {
            //if(contactGenHelper.workingState_.nbContacts > 3)
            //{
                rep.result_ = contactGenHelper.workingState_;
                rep.status_ = NO_CONTACT;
                rep.success_ = true;
                return rep;
            //}
        }
        for(std::vector<std::string>::const_iterator cit = cState.second.begin();
            cit != cState.second.end(); ++cit)
        {
            if(cit+1 == cState.second.end())
                contactGenHelper.checkStabilityGenerate_ = checkStability;
            rep = generate_contact(contactGenHelper,*cit);
            if(rep.success_)
            {
                contactGenHelper.workingState_ = rep.result_;
            }
            //else
            //    break;
        }
    }
    return rep;
}

projection::ProjectionReport repositionContacts(ContactGenHelper& helper)
{
    ProjectionReport resultReport;
    State result = helper.workingState_;
    result.stable = false;
    State previous = result;
    // replace existing contacts
    // start with older contact created
    std::stack<std::string> poppedContacts;
    std::queue<std::string> oldOrder = result.contactOrder_;
    std::queue<std::string> newOrder;
    std::string nContactName ="";
    core::Configuration_t savedConfig = helper.previousState_.configuration_;
    core::Configuration_t config = savedConfig;
    while(!result.stable &&  !oldOrder.empty())
    {
        std::string previousContactName = oldOrder.front();
        std::string groupName = helper.fullBody_->GetLimbs().at(previousContactName)->limb_->name();
        const std::vector<std::string>& group = helper.fullBody_->GetGroups().at(groupName);
        oldOrder.pop();
        core::ConfigurationIn_t save = helper.fullBody_->device_->currentConfiguration();
        bool notFound(true);
        for(std::vector<std::string>::const_iterator cit = group.begin();
            notFound && cit != group.end(); ++cit)
        {
            result.RemoveContact(*cit);
            helper.workingState_ = result;
            projection::ProjectionReport rep = contact::generate_contact(helper,*cit);
            if(rep.status_ == STABLE_CONTACT)
            {
                nContactName = *cit;
                notFound = false;
                result = rep.result_;
            }
            else
            {
                result = previous;
                config = savedConfig;
            }
        }
        if(notFound)
        {
            config = savedConfig;
            result.configuration_ = savedConfig;
            poppedContacts.push(previousContactName);
            helper.fullBody_->device_->currentConfiguration(save);
        }
    }
    while(!poppedContacts.empty())
    {
        newOrder.push(poppedContacts.top());
        poppedContacts.pop();
    }
    while(!oldOrder.empty())
    {
        newOrder.push(oldOrder.front());
        oldOrder.pop();
    }
    if(result.stable)
    {
        newOrder.push(nContactName);
        resultReport.status_ = STABLE_CONTACT;
        resultReport.success_ = true;
    }
    result.contactOrder_ = newOrder;
    resultReport.result_ = result;
    return resultReport;
}

} // namespace projection
} // namespace rbprm
} // namespace hpp
