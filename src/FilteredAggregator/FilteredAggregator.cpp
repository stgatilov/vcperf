#include "FilteredAggregator.h"
#include <algorithm>
#include <stdio.h>
#include "Wildcard.h"
#include "Undecorate.h"

using namespace Microsoft::Cpp::BuildInsights;
using namespace vcperf;


FilteringAggregator::FilteringAggregator(const std::string& wildcard)
{
    wildcard_ = wildcard;
}

void FilteringAggregator::WildcardTime::Print() const
{
    printf("  CPU Time:     %10.6lf / %10.6lf / %10.6lf\n", 1e-9 * exclusiveCpuTime_, 1e-9 * subtractedCpuTime_, 1e-9 * inclusiveCpuTime_);
    printf("  Duration:     %10.6lf / %10.6lf / %10.6lf\n", 1e-9 * exclusiveDuration_, 1e-9 * subtractedDuration_, 1e-9 * inclusiveDuration_);
}

BI::AnalysisControl FilteringAggregator::OnEndAnalysisPass()
{
    if (passNumber_ == 0) {
        std::sort(filteredKeys_.begin(), filteredKeys_.end());
        filteredKeys_.resize(std::unique(filteredKeys_.begin(), filteredKeys_.end()) - filteredKeys_.begin());

        std::sort(functionNamesDecorated_.begin(), functionNamesDecorated_.end());
        functionNamesDecorated_.resize(std::unique(functionNamesDecorated_.begin(), functionNamesDecorated_.end()) - functionNamesDecorated_.begin());
        Undecorate(functionNamesDecorated_, functionNamesUndecorated_);
    }
    else if (passNumber_ == 1) {
        filteredKeys_.clear();

        printf("Total time for parsing files matching \"%s\":\n", wildcard_.c_str());
        _parsingTime.Print();
        printf("Total time for template instantiations matching \"%s\":\n", wildcard_.c_str());
        _instantiationsTime.Print();
        printf("Total time for code generation matching \"%s\":\n", wildcard_.c_str());
        _generationTime.Print();
    }
    passNumber_++;

    return AnalysisControl::CONTINUE;
}

AnalysisControl FilteringAggregator::OnStopActivity(const EventStack& eventStack)
{
    if (passNumber_ == 1) {
        MatchEventStackInMemberFunction(eventStack, this, &FilteringAggregator::OnFinishTemplateInstantiation);
        MatchEventStackInMemberFunction(eventStack, this, &FilteringAggregator::OnFileParse);
    }
    MatchEventStackInMemberFunction(eventStack, this, &FilteringAggregator::OnFunction);

    return AnalysisControl::CONTINUE;
}

AnalysisControl FilteringAggregator::OnSimpleEvent(const EventStack& eventStack)
{
    if (passNumber_ == 0) {
        MatchEventStackInMemberFunction(eventStack, this, &FilteringAggregator::OnSymbolName);
    }

    return AnalysisControl::CONTINUE;
}

void FilteringAggregator::OnSymbolName(const SE::SymbolName& symbolName)
{
    const char *name = symbolName.Name();
    size_t len = strlen(name);
    if (WildcardMatch(wildcard_.c_str(), name)) {
        filteredKeys_.push_back(symbolName.Key());
    }
}

bool FilteringAggregator::InstantiationMatchesWildcard(const A::TemplateInstantiation& templateInstantiation) const
{
    uint64_t primaryKey = templateInstantiation.PrimaryTemplateSymbolKey();
    size_t pos = std::lower_bound(filteredKeys_.begin(), filteredKeys_.end(), primaryKey) - filteredKeys_.begin();
    return (pos < filteredKeys_.size() && filteredKeys_[pos] == primaryKey);
}

bool FilteringAggregator::FileParseMatchesWildcard(const A::FrontEndFile& file) const
{
    const char *path = file.Path();
    //bool isHeader = !(WildcardMatch("*.cpp", path) || WildcardMatch("*.cxx", path) || WildcardMatch("*.c", path));
    return WildcardMatch(wildcard_.c_str(), path);
}

bool FilteringAggregator::FunctionMatchesWildcard(const A::Function& function) const
{
    std::string decName = function.Name();
    size_t pos = std::lower_bound(functionNamesDecorated_.begin(), functionNamesDecorated_.end(), decName) - functionNamesDecorated_.begin();
    if (pos == functionNamesDecorated_.size() || functionNamesDecorated_[pos] != decName)
        return false;   //not even registered
    const std::string& undecName = functionNamesUndecorated_[pos];
    return WildcardMatch(wildcard_.c_str(), undecName.c_str());
}

template<class Activity> void FilteringAggregator::UpdateWildcardTime(const BI::EventGroup<Activity>& activityGroup, bool (FilteringAggregator::*matchFunc)(const Activity&) const, WildcardTime& totalTime) const
{
    int n = (int)activityGroup.Size();
    uint64_t inclCpuTime = activityGroup.Back().CPUTime().count();
    uint64_t inclDuration = activityGroup.Back().Duration().count();
    uint64_t exclCpuTime = activityGroup.Back().ExclusiveCPUTime().count();
    uint64_t exclDuration = activityGroup.Back().ExclusiveDuration().count();

    if (n >= 1 && (this->*matchFunc)(activityGroup[n-1])) {
        bool hasMatchingAncestor = false;
        for (int i = n-2; i >= 0; i--)
            if ((this->*matchFunc)(activityGroup[i])) {
                hasMatchingAncestor = true;
                break;
            }
        if (!hasMatchingAncestor) {
            //instantiation stack looks like:  no, no, no, ..., no, match
            totalTime.inclusiveCpuTime_ += inclCpuTime;
            totalTime.inclusiveDuration_ += inclDuration;
        }
        //instantiation stack looks like:  any, any, any, ..., any, match
        totalTime.subtractedCpuTime_ += inclCpuTime;
        totalTime.subtractedDuration_ += inclDuration;
        totalTime.exclusiveCpuTime_ += exclCpuTime;
        totalTime.exclusiveDuration_ +=  exclDuration;
    }
    if (n >= 2 && (this->*matchFunc)(activityGroup[n-2])) {
        //instantiation stack looks like:  any any ... any match any
        totalTime.subtractedCpuTime_ -= inclCpuTime;
        totalTime.subtractedDuration_ -= inclDuration;
    }
}

template<class Activity> void FilteringAggregator::UpdateWildcardTime(const Activity& activity, bool (FilteringAggregator::* matchFunc)(const Activity&) const, WildcardTime& totalTime) const
{
    if (!(this->*matchFunc)(activity))
        return;

    uint64_t inclCpuTime = activity.CPUTime().count();
    uint64_t inclDuration = activity.Duration().count();
    uint64_t exclCpuTime = activity.ExclusiveCPUTime().count();
    uint64_t exclDuration = activity.ExclusiveDuration().count();
    totalTime.inclusiveCpuTime_ += inclCpuTime;
    totalTime.inclusiveDuration_ += inclDuration;
    totalTime.subtractedCpuTime_ += inclCpuTime;
    totalTime.subtractedDuration_ += inclDuration;
    totalTime.exclusiveCpuTime_ += exclCpuTime;
    totalTime.exclusiveDuration_ += exclDuration;
}

void FilteringAggregator::OnFinishTemplateInstantiation(const A::Activity& parent, const A::TemplateInstantiationGroup& templateInstantiationGroup)
{
    UpdateWildcardTime(templateInstantiationGroup, &FilteringAggregator::InstantiationMatchesWildcard, _instantiationsTime);
}

void FilteringAggregator::OnFileParse(const A::FrontEndFileGroup& files)
{
    UpdateWildcardTime(files, &FilteringAggregator::FileParseMatchesWildcard, _parsingTime);
}

void FilteringAggregator::OnFunction(const A::Function& function)
{
    if (passNumber_ == 0) {
        functionNamesDecorated_.push_back(function.Name());
    }
    if (passNumber_ == 1) {
       UpdateWildcardTime(function, &FilteringAggregator::FunctionMatchesWildcard, _generationTime);
    }
}
