// generateStockPrice():
//
// Region:
//  name
//  id
//  tradeDayOfWeek
//  dstRangeStart
//  holidays[]
//  utc2localOffset
//  utc2localDSTOffset
//
//  utc2Local(time, region): date = time.date; return time + date < region.dstRangeStart ?  utc2localOffset : utc2localDSTOffset;
//  local2utc(time, region): date = time.date; return time - date < region.dstRangeStart ?  utc2localOffset : utc2localDSTOffset;
//
//
//  // not accounting for end date of dst as that's irrelevant to current problem
//  getUTCStartTime(runDate, region):
//      local2utc(runDate + region.localStartTimes)
//
// read region.yaml, 
// loop over each region
// while(region.nstocks--):
//   v = generateStockPrice(securityId, region)
int main(){

}
