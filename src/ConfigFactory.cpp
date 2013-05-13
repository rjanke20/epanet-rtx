//
//  ConfigFactory.cpp
//  epanet-rtx
//
//  Created by the EPANET-RTX Development Team
//  See README.md and license.txt for more information
//  

#include <iostream>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/lexical_cast.hpp>

#include "ConfigFactory.h"
#include "AggregatorTimeSeries.h"
#include "MovingAverage.h"
#include "Resampler.h"
#include "FirstDerivative.h"
#include "OffsetTimeSeries.h"
#include "ThresholdTimeSeries.h"
#include "CurveFunction.h"
#include "ConstantTimeSeries.h"
#include "MultiplierTimeSeries.h"
#include "ValidRangeTimeSeries.h"

#include "PointRecord.h"
#include "CsvPointRecord.h"

// conditional compilation
#ifndef RTX_NO_ODBC
  #include "OdbcPointRecord.h"
#endif
#ifndef RTX_NO_MYSQL
  #include "MysqlPointRecord.h"
#endif

#include "Zone.h"
#include "EpanetModel.h"
#include "EpanetSyntheticModel.h"

using namespace RTX;
using namespace libconfig;
using namespace std;

namespace RTX {
  class PointRecordFactory {
  public:
    static PointRecord::sharedPointer createCsvPointRecord(Setting& setting);
#ifndef RTX_NO_ODBC
    static PointRecord::sharedPointer createOdbcPointRecord(Setting& setting);
#endif
#ifndef RTX_NO_MYSQL
    static PointRecord::sharedPointer createMySqlPointRecord(Setting& setting);
#endif
  };
}


#pragma mark Constructor/Destructor

ConfigFactory::ConfigFactory() {
  // register point record and time series types to their proper creators
  _pointRecordPointerMap["CSV"] = PointRecordFactory::createCsvPointRecord;
  #ifndef RTX_NO_ODBC
  _pointRecordPointerMap["SCADA"] = PointRecordFactory::createOdbcPointRecord;
  #endif
  #ifndef RTX_NO_MYSQL
  _pointRecordPointerMap["MySQL"] = PointRecordFactory::createMySqlPointRecord;
  #endif
  
  //_clockPointerMap.insert(make_pair("regular", &ConfigFactory::createRegularClock));
  
  _timeSeriesPointerMap.insert(make_pair("TimeSeries", &ConfigFactory::createTimeSeries));
  _timeSeriesPointerMap.insert(make_pair("MovingAverage", &ConfigFactory::createMovingAverage));
  _timeSeriesPointerMap.insert(make_pair("Aggregator", &ConfigFactory::createAggregator));
  _timeSeriesPointerMap.insert(make_pair("Resampler", &ConfigFactory::createResampler));
  _timeSeriesPointerMap.insert(make_pair("Derivative", &ConfigFactory::createDerivative));
  _timeSeriesPointerMap.insert(make_pair("Offset", &ConfigFactory::createOffset));
  _timeSeriesPointerMap.insert(make_pair("FirstDerivative", &ConfigFactory::createDerivative));
  _timeSeriesPointerMap.insert(make_pair("Threshold", &ConfigFactory::createThreshold));
  _timeSeriesPointerMap.insert(make_pair("CurveFunction", &ConfigFactory::createCurveFunction));
  _timeSeriesPointerMap.insert(make_pair("Multiplier", &ConfigFactory::createMultiplier));
  _timeSeriesPointerMap.insert(make_pair("ValidRange", &ConfigFactory::createValidRange));
  _timeSeriesPointerMap.insert(make_pair("Constant", &ConfigFactory::createConstant));
  
  // node-type configuration functions
  // Junctions
  _parameterSetter.insert(make_pair("qualitysource", &ConfigFactory::configureQualitySource));
  _parameterSetter.insert(make_pair("quality", &ConfigFactory::configureQualityMeasure));
  _parameterSetter.insert(make_pair("boundaryflow", &ConfigFactory::configureBoundaryFlow));
  _parameterSetter.insert(make_pair("headmeasure", &ConfigFactory::configureHeadMeasure));
  _parameterSetter.insert(make_pair("pressuremeasure", &ConfigFactory::configurePressureMeasure));
  // Tanks, Reservoirs
  _parameterSetter.insert(make_pair("levelmeasure", &ConfigFactory::configureLevelMeasure));
  _parameterSetter.insert(make_pair("boundaryhead", &ConfigFactory::configureBoundaryHead));
  
  // link-type configuration functions
  // Pipes
  _parameterSetter.insert(make_pair("status", &ConfigFactory::configurePipeStatus));
  _parameterSetter.insert(make_pair("flow", &ConfigFactory::configureFlowMeasure));
  // Pumps
  _parameterSetter.insert(make_pair("curve", &ConfigFactory::configurePumpCurve));
  _parameterSetter.insert(make_pair("energy", &ConfigFactory::configurePumpEnergyMeasure));
  // Valves
  _parameterSetter.insert(make_pair("setting", &ConfigFactory::configureValveSetting));
  
  _doesHaveStateRecord = false;
  
}

ConfigFactory::~ConfigFactory() {
  _pointRecordPointerMap.clear();
  _timeSeriesPointerMap.clear();
  _timeSeriesList.clear();
  _clockList.clear();
  _pointRecordList.clear();
}

#pragma mark - Loading File

void ConfigFactory::loadConfigFile(const string& path) {
  
  _configPath = path;
  boost::filesystem::path configPath(path);
  
  // use libconfig api to open config file
  try
  {
    _configuration.readFile(configPath.string().c_str());
  }
  catch(const FileIOException &fioex)
  {
    cerr << "I/O error while reading file." << endl;
    return;
  }
  catch(const ParseException &pex)
  {
    cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine() << " - " << pex.getError() << endl;
    return;
  }
  
  // get the root setting node from the configuration
  Setting& root = _configuration.getRoot();
  
  // get the version number
  string version = _configuration.lookup("version");
  // todo -- check version number against CONFIGVERSION
  
  
  // access the settings.
  // open the config root, and get the length of the pointrecord group.
  // create the path if it doesn't exist yet.
  
  if ( !root.exists("configuration") ) {
    root.add("configuration", Setting::TypeGroup);
  }
  Setting& config = root["configuration"];
  
  // get the first set - point records.
  if ( !config.exists("records") ) {
    config.add("records", Setting::TypeList);
  } else {
    Setting& records = config["records"];
    createPointRecords(records);
  }
  
  // get clocks
  if ( !config.exists("clocks") ) {
    config.add("clocks", Setting::TypeList);
  } else {
    Setting& clockGroup = config["clocks"];
    createClocks(clockGroup);
  }
  
  // get timeseries
  if ( !config.exists("timeseries") ) {
    config.add("timeseries", Setting::TypeList);
  } else {
    Setting& timeSeriesGroup = config["timeseries"];
    createTimeSeriesList(timeSeriesGroup);
  }
  
  // make a new model
  if ( !config.exists("model") ) {
    config.add("model", Setting::TypeList);
  } else {
    Setting& modelGroup = config["model"];
    createModel(modelGroup);
  }
  
  // set simulation defaults
  if ( !config.exists("simulation") ) {
    config.add("simulation", Setting::TypeList);
  } else {
    Setting& simulationGroup = config["simulation"];
    createSimulationDefaults(simulationGroup);
  }
  
  // make zones
  if ( !config.exists("zones") ) {
    config.add("zones", Setting::TypeList);
  } else {
    Setting& zoneGroup = config["zones"];
    createZones(zoneGroup);
  }
  
  // data persistence
  if (!config.exists("save")) {
    config.add("save", Setting::TypeList);
  } else {
    Setting &saveGroup = config["save"];
    createSaveOptions(saveGroup);
  }
  
  
}


map<string, TimeSeries::sharedPointer> ConfigFactory::timeSeries() {
  return _timeSeriesList;
}

map<string, PointRecord::sharedPointer> ConfigFactory::pointRecords() {
  return _pointRecordList;
}

PointRecord::sharedPointer ConfigFactory::defaultRecord() {
  return _defaultRecord;
}

map<string, Clock::sharedPointer> ConfigFactory::clocks() {
  return _clockList;
}


#pragma mark - PointRecord

void ConfigFactory::createPointRecords(Setting& records) {  
  
  int recordCount = records.getLength();
  string recordName("");
  
  // loop through the records and create them.
  for (int iRecord = 0; iRecord < recordCount; ++iRecord) {
    Setting& record = records[iRecord];
    if (record.exists("name")) {
      record.lookupValue("name", recordName);
    }
    else {
      recordName = "Record " + boost::lexical_cast<std::string>(iRecord);
    }
    // somewhat hackish. since the pointrecords are created via static class methods, we have to piggy-back
    // the config file path onto the Setting& argument in case the pointrecord needs it (like the csv version will)
    record.add("configPath", libconfig::Setting::Type::TypeString);
    record["configPath"] = _configPath;
    PointRecord::sharedPointer pointRecord = createPointRecordOfType(record);
    if (pointRecord) {
      _pointRecordList[recordName] = pointRecord;
    }
    else {
      cerr << "could not load point record\n";
    }
    // strip the config path. we were never here.
    record.remove("configPath");
  }
  
  return;
}

// simple layer of indirection to make function pointer execution cleaner in the calling code.
// this just executes a function pointer stored in a map, which is keyed with the string name of the type of pointrecord to create.
// so access the "type" field of the passed setting, and execute the proper function to create the pointrecord.
PointRecord::sharedPointer ConfigFactory::createPointRecordOfType(libconfig::Setting &setting) {
  // check if the map item exists first
  string type;
  if (setting.lookupValue("type", type) && (_pointRecordPointerMap.find(type) != _pointRecordPointerMap.end()) ) {
    PointRecordFunctionPointer fp = _pointRecordPointerMap[type];
    return fp(setting);
  }
  
  cerr << "Point Record type [" << type << "] not supported" << endl;
  PointRecord::sharedPointer empty;
  return empty;
}


PointRecord::sharedPointer PointRecordFactory::createCsvPointRecord(Setting& setting) {
  CsvPointRecord::sharedPointer csv(new CsvPointRecord());
  string csvDirPath, name, configPath;
  
  if (setting.lookupValue("name", name) && setting.lookupValue("path", csvDirPath) && setting.lookupValue("configPath", configPath) ) {
    bool readOnly = false;
    if (setting.exists("readonly")) {
      setting.lookupValue("readonly", readOnly);
    }
    
    csv->setReadOnly(readOnly);
    
    boost::filesystem::path path(configPath);
    path = path.parent_path();
    path /= csvDirPath;
    csv->setPath(path.string());
    
  }
  else {
    cerr << "CSV Point Record -- check config" << endl;
  }
  
  return csv;
}

#pragma mark - Conditional DB Methods

#ifndef RTX_NO_ODBC

PointRecord::sharedPointer PointRecordFactory::createOdbcPointRecord(libconfig::Setting &setting) {
  OdbcPointRecord::sharedPointer r( new OdbcPointRecord() );
  // create the initialization string for the scada point record.
  string initString, name;
  if ( !setting.lookupValue("connection", initString) || !setting.lookupValue("name", name) ) {
    cerr << "odbc record name or connection not valid -- check config";
  }
  
  if (setting.exists("querySyntax")) {
    libconfig::Setting& syntax = setting["querySyntax"];
    string table    = syntax["Table"];
    string dateCol  = syntax["DateColumn"];
    string tagCol   = syntax["TagColumn"];
    string valueCol = syntax["ValueColumn"];
    string qualCol  = syntax["QualityColumn"];
    r->setTableColumnNames(table, dateCol, tagCol, valueCol, qualCol);
  }
  
  if (setting.exists("connectorType")) {
    // a pre-formatted connector type. yay!
    string type = setting["connectorType"];
    OdbcPointRecord::Sql_Connector_t connT = OdbcPointRecord::typeForName(type);
    if (connT != OdbcPointRecord::NO_CONNECTOR) {
      //cout << "connector type " << type << " recognized" << endl;
      r->setConnectorType(connT);
    }
    else {
      cerr << "connector type " << type << " not set" << endl;
    }
  }
  else {
    cerr << "connector type not specified" << endl;
  }
  
  r->setConnectionString(initString);  
  return r;
}

#endif

#ifndef RTX_NO_MYSQL

PointRecord::sharedPointer PointRecordFactory::createMySqlPointRecord(libconfig::Setting &setting) {
  string name = setting["name"];
  MysqlPointRecord::sharedPointer record( new MysqlPointRecord() );
  string initString = setting["connection"];
  record->setConnectionString(initString);
  //record->setName(name);
  //record->connect(); // leaving this to application code
  return record;
}

#endif



#pragma mark - Clocks

// keeping the clock creation simple for now - e.g., no function pointers.
void ConfigFactory::createClocks(Setting& clockGroup) {
  int clockCount = clockGroup.getLength();
  for (int iClock = 0; iClock < clockCount; ++iClock) {
    Setting& clock = clockGroup[iClock];
    string clockName = clock["name"];
    int period = clock["period"];
    Clock::sharedPointer aClock( new Clock(period) );
    _clockList[clockName] = aClock;
  }
  
  return;
}



#pragma mark - TimeSeries

void ConfigFactory::createTimeSeriesList(Setting& timeSeriesGroup) {  
  int tsCount = timeSeriesGroup.getLength();
  // loop through the time series and create them.
  for (int iSeries = 0; iSeries < tsCount; ++iSeries) {
    Setting& series = timeSeriesGroup[iSeries];
    string seriesName = series["name"];
    TimeSeries::sharedPointer theTimeSeries = createTimeSeriesOfType(series);
    if (theTimeSeries != NULL) {
      _timeSeriesList[seriesName] = theTimeSeries;
    }
    else {
      cerr << "could not create time series: " << seriesName << " -- check config." << endl;
    }
  }
  
  // connect single sources (ModularTimeSeries subclasses)
  typedef map<string, string> stringMap_t;
  BOOST_FOREACH(const stringMap_t::value_type& stringPair, _timeSeriesSourceList) {
    
    string tsName = stringPair.first;
    string sourceName = stringPair.second;
    
    if (_timeSeriesList.find(tsName) == _timeSeriesList.end()) {
      cerr << "cannot locate Timeseries " << tsName << endl;
      continue;
    }
    if (_timeSeriesList.find(sourceName) == _timeSeriesList.end()) {
      cerr << "cannot locate specified source Timeseries " << sourceName << endl;
      cerr << "-- (specified by Timeseries " << tsName << ")" << endl;
      continue;
    }
    
    ModularTimeSeries::sharedPointer ts = boost::static_pointer_cast<ModularTimeSeries>(_timeSeriesList[tsName]);
    TimeSeries::sharedPointer source = _timeSeriesList[sourceName];
    
    ts->setSource(source);
    
  }
  
  
  // connect multiplier time series sources
  typedef map<TimeSeries::sharedPointer,string> multiplierMap_t;
  BOOST_FOREACH(multiplierMap_t::value_type& multPair, _multiplierBasisList) {
    TimeSeries::sharedPointer ts = multPair.first;
    string basisName = multPair.second;
    
    if (_timeSeriesList.find(ts->name()) == _timeSeriesList.end()) {
      cerr << "cannot locate Timeseries " << ts->name() << endl;
      continue;
    }
    
    MultiplierTimeSeries::sharedPointer mts = boost::static_pointer_cast<MultiplierTimeSeries>(ts);
    if (mts) {
      mts->setMultiplier(_timeSeriesList[basisName]);
    }
    
  }
  
  
  typedef map<string, vector< pair<string, double> > > aggregatorMap_t;
  typedef vector<pair<string,double> > stringDoublePair_t;
  
  // go through the list of aggregator time series
  BOOST_FOREACH(const aggregatorMap_t::value_type& aggregatorPair, _timeSeriesAggregationSourceList) {
    
    string tsName = aggregatorPair.first;
    stringDoublePair_t aggregationList = aggregatorPair.second;
    
    if (_timeSeriesList.find(tsName) == _timeSeriesList.end()) {
      cerr << "cannot locate Timeseries " << tsName << endl;
      continue;
    }
    
    AggregatorTimeSeries::sharedPointer ts = boost::static_pointer_cast<AggregatorTimeSeries>(_timeSeriesList[tsName]);
    
    // go through the list and connect sources w/ multipliers
    BOOST_FOREACH(const stringDoublePair_t::value_type& entry, aggregationList) {
      string sourceName = entry.first;
      double multiplier = entry.second;
      
      if (_timeSeriesList.find(sourceName) == _timeSeriesList.end()) {
        cerr << "cannot locate specified source Timeseries " << sourceName << endl;
        cerr << "-- (specified by Timeseries " << tsName << ")" << endl;
        continue;
      }
      
      TimeSeries::sharedPointer source = _timeSeriesList[sourceName];
      
      ts->addSource(source, multiplier);
    }
  }
  
  return;
}

TimeSeries::sharedPointer ConfigFactory::createTimeSeriesOfType(libconfig::Setting &setting) {
  string type = setting["type"];
  if (_timeSeriesPointerMap.find(type) == _timeSeriesPointerMap.end()) {
    // not found
    cerr << "time series type " << type << " not implemented or not recognized" << endl;
    TimeSeries::sharedPointer empty;
    return empty;
  }
  TimeSeriesFunctionPointer fp = _timeSeriesPointerMap[type];
  return (this->*fp)(setting);
}

void ConfigFactory::setGenericTimeSeriesProperties(TimeSeries::sharedPointer timeSeries, libconfig::Setting &setting) {
  string myName = setting["name"];
  timeSeries->setName(myName);
  
  Units theUnits(RTX_DIMENSIONLESS);
  if (setting.exists("units")) {
    string unitName = setting["units"];
    theUnits = Units::unitOfType(unitName);
  }
  timeSeries->setUnits(theUnits);
  
  if (setting.exists("clock")) {
    Clock::sharedPointer clock = _clockList[setting["clock"]];
    timeSeries->setClock(clock);
  }
  
  // if a pointRecord is specified, then re-set the timeseries' cache.
  // this means that the storage for the time series is probably external (scada / mysql).
  if (setting.exists("pointRecord")) {
    // TODO - test for existence of the actual point record.
    string pointRecordName = setting["pointRecord"];
    PointRecord::sharedPointer pointRecord = _pointRecordList[setting["pointRecord"]];
    timeSeries->setRecord(pointRecord);
  }
  
  // set any upstream sources. forward declarations are allowed, as these will be set only after all timeseries objects have been created.
  if (setting.exists("source")) {
    // this timeseries has an upstream source.
    string sourceName = setting["source"];
    _timeSeriesSourceList[myName] = sourceName;
  }
  
  // TODO -- description field
  
  
  
}

TimeSeries::sharedPointer ConfigFactory::createTimeSeries(libconfig::Setting &setting) {
  TimeSeries::sharedPointer timeSeries( new TimeSeries() );
  setGenericTimeSeriesProperties(timeSeries, setting);
  return timeSeries;
}

TimeSeries::sharedPointer ConfigFactory::createAggregator(libconfig::Setting &setting) {
  AggregatorTimeSeries::sharedPointer timeSeries( new AggregatorTimeSeries() );
  // set generic properties
  setGenericTimeSeriesProperties(timeSeries, setting);
  // additional setters for this class...
  // a list of sources with multipliers.
  Setting& sources = setting["sources"];
  int sourceCount = sources.getLength();
  
  // create a vector for this list
  vector<pair<string, double> >sourceList;
  
  for (int iSource = 0; iSource < sourceCount; ++iSource) {
    Setting& thisSource = sources[iSource];
    string sourceName = thisSource["source"];
    double multiplier;
    // set the multiplier if it's specified - otherwise, set to 1.
    if (thisSource.exists("multiplier")) {
      multiplier = getConfigDouble(thisSource, "multiplier");
    }
    else {
      multiplier = 1.;
    }
    sourceList.push_back(make_pair(sourceName, multiplier));
  }
  
  
  _timeSeriesAggregationSourceList[timeSeries->name()] = sourceList;
  
  return timeSeries;
}

TimeSeries::sharedPointer ConfigFactory::createMovingAverage(libconfig::Setting &setting) {
  MovingAverage::sharedPointer timeSeries( new MovingAverage() );
  // set generic properties
  setGenericTimeSeriesProperties(timeSeries, setting);
  
  // class-specific settings
  int window = setting["window"];
  timeSeries->setWindowSize(window);
  
  TimeSeries::sharedPointer returnTS = timeSeries;
  return returnTS;
}

TimeSeries::sharedPointer ConfigFactory::createResampler(libconfig::Setting &setting) {
  Resampler::sharedPointer resampler( new Resampler() );
  setGenericTimeSeriesProperties(resampler, setting);
  return resampler;
}

TimeSeries::sharedPointer ConfigFactory::createDerivative(Setting &setting) {
  FirstDerivative::sharedPointer derivative( new FirstDerivative() );
  setGenericTimeSeriesProperties(derivative, setting);
  return derivative;
}

TimeSeries::sharedPointer ConfigFactory::createOffset(Setting &setting) {
  OffsetTimeSeries::sharedPointer offset( new OffsetTimeSeries() );
  setGenericTimeSeriesProperties(offset, setting);
  if (setting.exists("offsetValue")) {
    double v = getConfigDouble(setting, "offsetValue");
    offset->setOffset(v);
  }
  
  return offset;
}

TimeSeries::sharedPointer ConfigFactory::createThreshold(Setting &setting) {
  ThresholdTimeSeries::sharedPointer status( new ThresholdTimeSeries() );
  setGenericTimeSeriesProperties(status, setting);
  if (setting.exists("thresholdValue")) {
    double v = getConfigDouble(setting, "thresholdValue");
    status->setThreshold(v);
  }

  return status;
}

TimeSeries::sharedPointer ConfigFactory::createCurveFunction(libconfig::Setting &setting) {
  CurveFunction::sharedPointer timeSeries( new CurveFunction() );
  // set generic properties
  setGenericTimeSeriesProperties(timeSeries, setting);
  
  // additional setters for this class...
  // input units
  Units theUnits(RTX_DIMENSIONLESS);
  if (setting.exists("inputUnits")) {
    string unitName = setting["inputUnits"];
    theUnits = Units::unitOfType(unitName);
  }
  timeSeries->setInputUnits(theUnits);

  // a list of (x,y) coordinates defining the curve.
  Setting& coordinates = setting["function"];
  int coordinateCount = coordinates.getLength();
  
  for (int iCoordinate = 0; iCoordinate < coordinateCount; ++iCoordinate) {
    Setting& thisCoordinate = coordinates[iCoordinate];
    
    if (thisCoordinate.exists("x") && thisCoordinate.exists("y")) {
      double x = getConfigDouble(thisCoordinate, "x");
      double y = getConfigDouble(thisCoordinate, "y");
      timeSeries->addCurveCoordinate(x, y);
    }

  }
  
  return timeSeries;
}

TimeSeries::sharedPointer ConfigFactory::createConstant(Setting &setting) {
  ConstantTimeSeries::sharedPointer constant( new ConstantTimeSeries() );
  setGenericTimeSeriesProperties(constant, setting);
  
  if (setting.exists("value")) {
    double val = getConfigDouble(setting, "value");
    constant->setValue(val);
  }
  
  return constant;
}


TimeSeries::sharedPointer ConfigFactory::createValidRange(Setting &setting) {
  
  ValidRangeTimeSeries::sharedPointer ts( new ValidRangeTimeSeries() );
  setGenericTimeSeriesProperties(ts, setting);
  
  pair<double,double> range = ts->range();
  if (setting.exists("range_min")) {
    range.first = getConfigDouble(setting, "range_min");
  }
  if (setting.exists("range_max")) {
    range.second = getConfigDouble(setting, "range_max");
  }
  string mode;
  if (setting.lookupValue("mode", mode)) {
    if (RTX_STRINGS_ARE_EQUAL(mode, "drop")) {
      ts->setMode(ValidRangeTimeSeries::drop);
    }
    else if (RTX_STRINGS_ARE_EQUAL(mode, "saturate")) {
      ts->setMode(ValidRangeTimeSeries::saturate);
    }
    else {
      cerr << "could not resolve mode: " << mode << " -- check config" << endl;
    }
  }
  
  
  ts->setRange(range.first, range.second);
  
  return ts;
}

TimeSeries::sharedPointer ConfigFactory::createMultiplier(Setting &setting) {
  
  MultiplierTimeSeries::sharedPointer ts( new MultiplierTimeSeries() );
  setGenericTimeSeriesProperties(ts, setting);
  
  string basis;
  if (setting.lookupValue("multiplier", basis)) {
    // add this ts:string pair to the map to be connected later.
    _multiplierBasisList[ts] = basis;
  }
  
  return ts;
}







double ConfigFactory::getConfigDouble(libconfig::Setting &config, const string &name) {
  double value = 0;
  if (!config.lookupValue(name, value)) {
    int iv;
    config.lookupValue(name, iv);
    value = (double)iv;
  }
  return value;
}

#pragma mark - Model

void ConfigFactory::createModel(Setting& setting) {
  
  string modelType = setting["type"];
  string modelFileName = setting["file"];
  boost::filesystem::path configPath(_configPath);
  boost::filesystem::path modelPath = configPath.parent_path();
  modelPath /= modelFileName;
  
  if ( RTX_STRINGS_ARE_EQUAL(modelType, "epanet") ){
    _model.reset( new EpanetModel() );    
    // load the model
    _model->loadModelFromFile(modelPath.string());
    // hook up the model's elements to timeseries objects
    _model->overrideControls();
    configureElements(_model->elements());
  }
  
  if ( RTX_STRINGS_ARE_EQUAL(modelType, "synthetic_epanet") ) {
    _model.reset( new EpanetSyntheticModel() );
    _model->loadModelFromFile(modelPath.string());
    configureElements(_model->elements());
  }
  

}

Model::sharedPointer ConfigFactory::model() {
  return _model;
}

#pragma mark - Simulation Settings

void ConfigFactory::createSimulationDefaults(Setting& setting) {
  // get simulation settings
  Setting& timeSetting = setting["time"];
  const int hydStep = timeSetting["hydraulic"];
  const int qualStep = timeSetting["quality"];
  
  // set other sim properties...
  _model->setHydraulicTimeStep(hydStep);
  _model->setQualityTimeStep(qualStep);
}

#pragma mark - Zone Settings

void ConfigFactory::createZones(Setting& zoneGroup) {
  bool detectClosed = false;
  // get the zone information from the config,
  // then create each zone and add it to the model.
  if ( zoneGroup.exists("auto_detect") ) {
    bool autoDetect = zoneGroup["auto_detect"];
    zoneGroup.lookupValue("detect_closed_links", detectClosed);
    if (autoDetect) {
      _model->initDemandZones(detectClosed);
    }
  }
  
}


#pragma mark - Save Options

void ConfigFactory::createSaveOptions(libconfig::Setting &saveGroup) {
  if (saveGroup.exists("staterecord")) {
    _doesHaveStateRecord = true;
    string defaultRecordName = saveGroup["staterecord"];
    if (_pointRecordList.find(defaultRecordName) == _pointRecordList.end()) {
      cerr << "could not retrieve point record by name: " << defaultRecordName << endl;
    }
    _defaultRecord = _pointRecordList[defaultRecordName];
    // provide the model object with this record
    
    // get the states we want to persist
    if (saveGroup.exists("save_states")) {
      Setting &saveSetting = saveGroup["save_states"];
      if (!saveSetting.isList()) {
        cerr << "save_states should be a list: check config format" << endl;
        return;
      }
      int nStates = saveSetting.getLength();
      for (int iState = 0; iState < nStates; ++iState) {
        string stateToSave = saveSetting[iState];
        if (RTX_STRINGS_ARE_EQUAL(stateToSave, "all")) {
          _model->setStorage(_defaultRecord);
        }
        else if (RTX_STRINGS_ARE_EQUAL(stateToSave, "measured")) {
          // save only the element states that have measured counterparts.
          vector<Junction::sharedPointer> junctions = _model->junctions();
          BOOST_FOREACH(Junction::sharedPointer j, junctions) {
            if (j->doesHaveHeadMeasure()) {
              j->head()->setRecord(_defaultRecord);
            }
            if (j->doesHaveQualityMeasure()) {
              j->quality()->setRecord(_defaultRecord);
            }
          }
          
          vector<Pipe::sharedPointer> pipes = _model->pipes();
          BOOST_FOREACH(Pipe::sharedPointer p, pipes) {
            if (p->doesHaveFlowMeasure()) {
              p->flow()->setRecord(_defaultRecord);
            }
          }
        } // measured
        else if (RTX_STRINGS_ARE_EQUAL(stateToSave, "zone_demand")) {
          vector<Zone::sharedPointer> zones = _model->zones();
          BOOST_FOREACH(Zone::sharedPointer z, zones) {
            z->setRecord(_defaultRecord);
          }
        } // zone demand
      } // list of states
    } // save_states group
    
    
  }
  else {
    cout << "Warning: no state record specified. Model results will not be persisted!" << endl;
  }
}


#pragma mark - Element Configuration


void ConfigFactory::configureElements(vector<Element::sharedPointer> elements) {
  BOOST_FOREACH(Element::sharedPointer element, elements) {
    //cout << "configuring " << element->name() << endl;
    configureElement(element);
  }
}

void ConfigFactory::configureElement(Element::sharedPointer element) {
  // make sure that the element is specified in the config file.
  string name = element->name();
  
  // find the "elements" section in the configuration
  if (!_configuration.exists("configuration.elements")) {
    return;
  }
  Setting& elements = _configuration.lookup("configuration.elements");
  
  // see if the elements list includes this element
  const int elementCount = elements.getLength();
  for (int iElement = 0; iElement < elementCount; ++iElement) {
    Setting& elementSetting = elements[iElement];
    string modelID = elementSetting["model_id"];
    if ( RTX_STRINGS_ARE_EQUAL(modelID, name) ) {
      // great, a match.
      // configure the element with the proper states/parameters.
      // todo - check element type (link or node)... names may not be unique.
      // todo - check if the type is in the pointer map
      
      // get the type of parameter
      if (!elementSetting.exists("parameter")) {
        cerr << "skipping element " << modelID << " : missing parameter" << endl;
      }
      string parameterType = elementSetting["parameter"];
      if (_parameterSetter.find(parameterType) == _parameterSetter.end()) {
        // no such parameter type
        cout << "could not find paramter type: " << parameterType << endl;
        return;
      }
      ParameterFunction fp = _parameterSetter[parameterType];
      const string tsName = elementSetting["timeseries"];
      TimeSeries::sharedPointer series = _timeSeriesList[tsName];
      if (!series) {
        cerr << "could not find time series \"" << tsName << "\"." << endl;
        return;
      }
      // configure the individual element using this setting.
      (this->*fp)(elementSetting, element);
    }
  }
}


#pragma mark Specific element configuration

void ConfigFactory::configureQualitySource(Setting &setting, Element::sharedPointer junction) {
  Junction::sharedPointer thisJunction = boost::dynamic_pointer_cast<Junction>(junction);
  if (thisJunction) {
    TimeSeries::sharedPointer quality = _timeSeriesList[setting["timeseries"]];
    thisJunction->setQualitySource(quality);
  }
}

void ConfigFactory::configureBoundaryFlow(Setting &setting, Element::sharedPointer junction) {
  Junction::sharedPointer thisJunction = boost::dynamic_pointer_cast<Junction>(junction);
  if (thisJunction) {
    TimeSeries::sharedPointer flow = _timeSeriesList[setting["timeseries"]];
    thisJunction->setBoundaryFlow(flow);
  }
}

void ConfigFactory::configureHeadMeasure(Setting &setting, Element::sharedPointer junction) {
  Junction::sharedPointer thisJunction = boost::dynamic_pointer_cast<Junction>(junction);
  if (thisJunction) {
    TimeSeries::sharedPointer head = _timeSeriesList[setting["timeseries"]];
    // if it's in units of PSI, then it requires a tweak. (TODO)
    thisJunction->setHeadMeasure(head);
  }
}

void ConfigFactory::configurePressureMeasure(Setting &setting, Element::sharedPointer junction) {
  Junction::sharedPointer thisJunction = boost::dynamic_pointer_cast<Junction>(junction);
  if (thisJunction) {
    TimeSeries::sharedPointer pres = _timeSeriesList[setting["timeseries"]];
    // if it's in units of PSI, then it requires a tweak. (TODO)
    thisJunction->setPressureMeasure(pres);
  }
}

void ConfigFactory::configureLevelMeasure(Setting &setting, Element::sharedPointer tank) {
  Tank::sharedPointer thisTank = boost::dynamic_pointer_cast<Tank>(tank);
  if (thisTank) {
    TimeSeries::sharedPointer level = _timeSeriesList[setting["timeseries"]];
    thisTank->setLevelMeasure(level);
  }
}

void ConfigFactory::configureQualityMeasure(Setting &setting, Element::sharedPointer junction) {
  Junction::sharedPointer thisJunction = boost::dynamic_pointer_cast<Junction>(junction);
  if (thisJunction) {
    TimeSeries::sharedPointer quality = _timeSeriesList[setting["timeseries"]];
    thisJunction->setQualityMeasure(quality);
  }
}

void ConfigFactory::configureBoundaryHead(Setting &setting, Element::sharedPointer reservoir) {
  Reservoir::sharedPointer thisReservoir = boost::dynamic_pointer_cast<Reservoir>(reservoir);
  if (thisReservoir) {
    TimeSeries::sharedPointer head = _timeSeriesList[setting["timeseries"]];
    thisReservoir->setBoundaryHead(head);
  }
}

void ConfigFactory::configurePipeStatus(Setting &setting, Element::sharedPointer pipe) {
  Pipe::sharedPointer thisPipe = boost::dynamic_pointer_cast<Pipe>(pipe);
  if (thisPipe) {
    TimeSeries::sharedPointer status = _timeSeriesList[setting["timeseries"]];
    thisPipe->setStatusParameter(status);
  }
}

void ConfigFactory::configureFlowMeasure(Setting &setting, Element::sharedPointer pipe) {
  Pipe::sharedPointer thisPipe = boost::dynamic_pointer_cast<Pipe>(pipe);
  if (thisPipe) {
    TimeSeries::sharedPointer flow = _timeSeriesList[setting["timeseries"]];
    thisPipe->setFlowMeasure(flow);
  }
}

void ConfigFactory::configurePumpCurve(Setting &setting, Element::sharedPointer pump) {
  Pump::sharedPointer thisPump = boost::dynamic_pointer_cast<Pump>(pump);
  if (thisPump) {
    TimeSeries::sharedPointer curve = _timeSeriesList[setting["timeseries"]];
    thisPump->setCurveParameter(curve);
  }
}

void ConfigFactory::configurePumpEnergyMeasure(Setting &setting, Element::sharedPointer pump) {
  Pump::sharedPointer thisPump = boost::dynamic_pointer_cast<Pump>(pump);
  if (thisPump) {
    TimeSeries::sharedPointer energy = _timeSeriesList[setting["timeseries"]];
    thisPump->setEnergyMeasure(energy);
  }
}
void ConfigFactory::configureValveSetting(Setting &setting, Element::sharedPointer valve) {
  Valve::sharedPointer thisValve = boost::dynamic_pointer_cast<Valve>(valve);
  if (thisValve) {
    TimeSeries::sharedPointer valveSetting = _timeSeriesList[setting["timeseries"]];
    thisValve->setSettingParameter(valveSetting);
  }
}








