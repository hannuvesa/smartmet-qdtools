// ======================================================================
/*!
 * \brief HDF5 to querydata conversion for EUMETNET OPERA data
 *
 * http://www.knmi.nl/opera/opera3/OPERA_2008_03_WP2.1b_ODIM_H5_v2.1.pdf
 */
// ======================================================================

#include <MXA/HDF5/H5Lite.h>
#include <MXA/HDF5/H5Utilities.h>

#include <macgyver/StringConversion.h>
#include <macgyver/TimeParser.h>

#include <newbase/NFmiAreaFactory.h>
#include <newbase/NFmiEnumConverter.h>
#include <newbase/NFmiEquidistArea.h>
#include <newbase/NFmiFastQueryInfo.h>
#include <newbase/NFmiGrid.h>
#include <newbase/NFmiHPlaceDescriptor.h>
#include <newbase/NFmiLevelType.h>
#include <newbase/NFmiMercatorArea.h>
#include <newbase/NFmiParamDescriptor.h>
#include <newbase/NFmiQueryData.h>
#include <newbase/NFmiQueryDataUtil.h>
#include <newbase/NFmiStereographicArea.h>
#include <newbase/NFmiTimeDescriptor.h>
#include <newbase/NFmiTimeList.h>
#include <newbase/NFmiVPlaceDescriptor.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>

#include <iostream>
#include <numeric>
#include <string>
#include <vector>

// Global to get better error messages outside param descriptor builder

NFmiEnumConverter converter;

// ----------------------------------------------------------------------
/*!
 * \brief Container for command line options
 */
// ----------------------------------------------------------------------

struct Options
{
  Options();

  bool verbose;              // -v --verbose
  std::string projection;    // -P --projection
  std::string infile;        // -i --infile
  std::string outfile;       // -o --outfile
  std::string datasetname;   // --datasetname
  std::string producername;  // --producername
  long producernumber;       // --producernumber
};

Options options;

// ----------------------------------------------------------------------
/*!
 * \brief Default options
 */
// ----------------------------------------------------------------------

Options::Options()
    : verbose(false),
      projection(""),
      infile("-"),
      outfile("-"),
      datasetname("dataset"),
      producername("RADAR"),
      producernumber(1014)
{
}
// ----------------------------------------------------------------------
/*!
 * \brief Parse command line options
 *
 * \return True, if execution may continue as usual
 */
// ----------------------------------------------------------------------

bool parse_options(int argc, char *argv[])
{
  namespace po = boost::program_options;
  namespace fs = boost::filesystem;

  std::string producerinfo;

  po::options_description desc("Allowed options");
  desc.add_options()("help,h", "print out help message")(
      "verbose,v", po::bool_switch(&options.verbose), "set verbose mode on")(
      "version,V", "display version number")(
      "projection,P", po::value(&options.projection), "projection")(
      "infile,i", po::value(&options.infile), "input HDF5 file")(
      "outfile,o", po::value(&options.outfile), "output querydata file")(
      "datasetname", po::value(&options.datasetname), "dataset name prefix (default=dataset)")(
      "producer,p", po::value(&producerinfo), "producer number,name")(
      "producernumber", po::value(&options.producernumber), "producer number (default: 1014)")(
      "producername", po::value(&options.producername), "producer name (default: RADAR)");

  po::positional_options_description p;
  p.add("infile", 1);
  p.add("outfile", 1);

  po::variables_map opt;
  po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), opt);

  po::notify(opt);

  if (opt.count("version") != 0)
  {
    std::cout << "h52qd v1.2 (" << __DATE__ << ' ' << __TIME__ << ')' << std::endl;
  }

  if (opt.count("help"))
  {
    std::cout << "Usage: h5toqd [options] infile outfile" << std::endl
              << std::endl
              << "Converts EUMETNET OPERA radar files to querydata." << std::endl
              << "Only features in known use are supported." << std::endl
              << std::endl
              << desc << std::endl;
    return false;
  }

  if (opt.count("infile") == 0) throw std::runtime_error("Expecting input file as parameter 1");

  if (opt.count("outfile") == 0) throw std::runtime_error("Expecting output file as parameter 2");

  if (!fs::exists(options.infile))
    throw std::runtime_error("Input file '" + options.infile + "' does not exist");

  // Handle the alternative ways to define the producer

  if (!producerinfo.empty())
  {
    std::vector<std::string> parts;
    boost::algorithm::split(parts, producerinfo, boost::algorithm::is_any_of(","));
    if (parts.size() != 2)
      throw std::runtime_error("Option --producer expects a comma separated number,name argument");

    options.producernumber = Fmi::stol(parts[0]);
    options.producername = parts[1];
  }

  return true;
}

// ----------------------------------------------------------------------
/*!
 * \brief Construct NFmiMetTime from posix time
 */
// ----------------------------------------------------------------------

NFmiMetTime tomettime(const boost::posix_time::ptime &t)
{
  return NFmiMetTime(t.date().year(),
                     t.date().month(),
                     t.date().day(),
                     t.time_of_day().hours(),
                     t.time_of_day().minutes(),
                     t.time_of_day().seconds(),
                     1);
}
// ----------------------------------------------------------------------
/*!
 * \brief Convert attribute value to string
 */
// ----------------------------------------------------------------------

std::string get_string(const std::string &name, IMXAArray &attr)
{
  if (attr.getDataType() != H5T_STRING) throw std::runtime_error(name + " is not a string");

  char *s = static_cast<char *>(attr.getVoidPointer(0));
  hsize_t slen = attr.getNumberOfElements();

  if (s[slen - 1] == 0) --slen;  // ignore \0 of null terminated strings

  return std::string(s, slen);
}

// ----------------------------------------------------------------------
/*!
 * \brief Convert numeric attribute value to string
 */
// ----------------------------------------------------------------------

template <typename T>
std::string get_string(const std::string &name, IMXAArray &attr)
{
  std::ostringstream out;

  hsize_t n = attr.getNumberOfElements();

  if (n != 1)
    throw std::runtime_error("Element " + name + " is not of size 1, but " +
                             boost::lexical_cast<std::string>(n));

  T *value = static_cast<T *>(attr.getVoidPointer(0));

  out << value[0];

  return out.str();
}

// ----------------------------------------------------------------------
/*!
 * \brief Convert attribute value to string
 */
// ----------------------------------------------------------------------

std::string get_attribute_string(const std::string &name, IMXAArray &attr)
{
  // Cannot use a switch here because H5T typenames are functions!

  int32_t id = attr.getDataType();

  if (id == H5T_STRING) return get_string(name, attr);
  if (id == H5T_NATIVE_FLOAT) return get_string<float>(name, attr);
  if (id == H5T_NATIVE_DOUBLE) return get_string<double>(name, attr);
  if (id == H5T_NATIVE_INT8) return get_string<int8_t>(name, attr);
  if (id == H5T_NATIVE_UINT8) return get_string<uint8_t>(name, attr);
  if (id == H5T_NATIVE_INT16) return get_string<int16_t>(name, attr);
  if (id == H5T_NATIVE_UINT16) return get_string<uint16_t>(name, attr);
  if (id == H5T_NATIVE_INT32) return get_string<int32_t>(name, attr);
  if (id == H5T_NATIVE_UINT32) return get_string<uint32_t>(name, attr);
  if (id == H5T_NATIVE_INT64) return get_string<int64_t>(name, attr);
  if (id == H5T_NATIVE_UINT64) return get_string<uint64_t>(name, attr);

  throw std::runtime_error("Variable " + name + " is of unknown type");
}

// ----------------------------------------------------------------------
/*!
 * \brief Get attribute value
 */
// ----------------------------------------------------------------------

template <typename T>
T get_attribute_value(const hid_t &hid, const std::string &path, const std::string &name)
{
  T value;

  if (H5Lite::readScalarAttribute(hid, path, name, value) != 0)
    throw std::runtime_error("Failed to read attribute " + path + "/" + name);

  return value;
}

// ----------------------------------------------------------------------
/*!
 * \brief Specialization for strings
 */
// ----------------------------------------------------------------------

template <>
std::string get_attribute_value<std::string>(const hid_t &hid,
                                             const std::string &path,
                                             const std::string &name)
{
  std::string value;

  if (H5Lite::readStringAttribute(hid, path, name, value) != 0)
    throw std::runtime_error("Failed to read attribute " + path + "/" + name);

  return value;
}

// ----------------------------------------------------------------------
/*!
 * \brief Get the most local attribute by name
 *
 * Searches for the named attribute in the entire tree specified by parent_path with group
 * specified by group_name. Searches from the most local group first.
 */
// ----------------------------------------------------------------------

template <typename T>
T get_attribute(const hid_t &hid,
                std::string parent_path,
                const std::string &group_name,
                const std::string &attribute_name)
{
  namespace fs = boost::filesystem;

  if (!boost::starts_with(parent_path, "/"))
  {
    parent_path.insert(0, "/");
  }

  // path must be the full path to the group parent in which the attribute is supposed to be.
  std::vector<fs::path> cumulativePaths;
  fs::path inPath(parent_path), cumulativePath, groupPath;

  while (!inPath.empty())
  {
    cumulativePaths.push_back(inPath);
    inPath = inPath.parent_path();
  }

  for (std::vector<fs::path>::iterator it = cumulativePaths.begin(); it != cumulativePaths.end();
       ++it)
  {
    groupPath = (*it) / group_name;
    std::string pathString = groupPath.string();
    if (H5Utilities::probeForAttribute(hid, pathString, attribute_name))
    {
      // Found attribute with given name
      return get_attribute_value<T>(hid, pathString, attribute_name);
    }
  }

  // Did not find an attribute with given group + name combination
  std::string errStr;
  errStr += "Did not find attribute: ";
  errStr += attribute_name;
  errStr += " with group: ";
  errStr += group_name;
  throw std::runtime_error(errStr);
}

// ----------------------------------------------------------------------
/*!
 * \brief Optional double values
 */
// ----------------------------------------------------------------------

boost::optional<double> get_optional_double(const hid_t &hid,
                                            std::string path,
                                            const std::string &name)
{
  try
  {
    return get_attribute_value<double>(hid, path, name);
  }
  catch (...)
  {
    return {};
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Optional double values
 */
// ----------------------------------------------------------------------

boost::optional<double> get_optional_double(const hid_t &hid,
                                            std::string parent_path,
                                            const std::string &group_name,
                                            const std::string &attribute_name)
{
  try
  {
    return get_attribute<double>(hid, parent_path, group_name, attribute_name);
  }
  catch (...)
  {
    return {};
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Test existence of attribute
 */
// ----------------------------------------------------------------------

template <typename T>
bool is_group_attribute(const hid_t &hid, const std::string &path, const std::string &name)
{
  herr_t hid_group = H5Lite::openId(hid, path, H5G_GROUP);

  herr_t ret = H5Lite::findAttribute(hid_group, name);

  H5Lite::closeId(hid_group, H5G_GROUP);

  return ret != 0;
}

template <typename T>
bool is_attribute(const hid_t &hid, const std::string &path, const std::string &name)
{
  T value;

  return (H5Lite::readScalarAttribute(hid, path, name, value) == 0);
}

template <>
bool is_attribute<std::string>(const hid_t &hid, const std::string &path, const std::string &name)
{
  std::string value;

  return (H5Lite::readStringAttribute(hid, path, name, value) == 0);
}

// ----------------------------------------------------------------------
/*!
 * \brief Get top level data names
 */
// ----------------------------------------------------------------------

std::list<std::string> get_top_names(const hid_t &hid)
{
  std::list<std::string> names;

#if 0
  // This works with HDF5 1.6.10, not with v1.8.5
  H5Utilities::getGroupObjects(hid,H5G_GROUP,names);
#else
  H5Utilities::getGroupObjects(hid, 1, names);
#endif

  return names;
}

// ----------------------------------------------------------------------
/*!
 * \brief Form a dataset name
 */
// ----------------------------------------------------------------------

std::string dataset(int index)
{
  return "/" + options.datasetname + boost::lexical_cast<std::string>(index);
}

// ----------------------------------------------------------------------
/*!
 * \brief Validate the HDF to be radar data
 *
 * Note: top level how group is optional
 */
// ----------------------------------------------------------------------

void validate_hdf(const hid_t &hid)
{
  std::list<std::string> names = get_top_names(hid);

  if (find(names.begin(), names.end(), "what") == names.end())
    throw std::runtime_error("Opera HDF5 radar data is required to contain a /what group");

  if (!H5Utilities::probeForAttribute(hid, "/what", "date"))
    throw std::runtime_error("Opera HDF5 radar data is required to contain /what.date attribute");

  if (!H5Utilities::probeForAttribute(hid, "/what", "time"))
    throw std::runtime_error("Opera HDF5 radar data is required to contain /what.time attribute");

  if (find(names.begin(), names.end(), options.datasetname + "1") == names.end())
    throw std::runtime_error("Opera HDF5 radar data is required to contain at least " + dataset(1) +
                             " group");

  if (find(names.begin(), names.end(), "where") == names.end())
    throw std::runtime_error("Opera HDF5 radar data is required to contain a /where group");
}

// ----------------------------------------------------------------------
/*!
 * \brief Count the number of datasets in the HDF
 *
 * Unfortunately there is no meta information for this, one has to
 * explicitly test whether the desired dataset exists.
 */
// ----------------------------------------------------------------------

int count_datasets(const hid_t &hid)
{
  std::list<std::string> names = get_top_names(hid);

  int n = 0;
  while (true)
  {
    std::string name = options.datasetname + boost::lexical_cast<std::string>(n + 1);
    if (find(names.begin(), names.end(), name) == names.end()) return n;
    ++n;
  }
  // NOT REACHED
}

// ----------------------------------------------------------------------
/*!
 * \brief Count the number of parameters in a dataset
 */
// ----------------------------------------------------------------------

int count_datas(const hid_t &hid, int i)
{
  const std::string prefix = dataset(i);

  hid_t gid = H5Utilities::openHDF5Object(hid, prefix);

  int counter(0);
  if (gid)
  {
    std::list<std::string> grpnames(get_top_names(gid));
    if (grpnames.size() == 0) return 0;

    std::set<std::string> grpset;
    for (std::list<std::string>::iterator it = grpnames.begin(); it != grpnames.end(); ++it)
      grpset.insert(grpset.end(), *it);

    for (int j = 1;; j++)
    {
      std::string dataname = std::string("data") + boost::lexical_cast<std::string>(j);
      if (grpset.find(dataname) != grpset.end())
      {
        counter++;
        continue;
      }
      break;
    }
  }

  return counter;

#ifdef OLD_IMPLEMENTATION
  // H5Utilities::isGroup() writes error message to stdout if group is not found
  const std::string prefix = dataset(i) + "/";

  for (int j = 1;; j++)
  {
    std::string dataprefix = prefix + "data" + boost::lexical_cast<std::string>(j);
    if (!H5Utilities::isGroup(hid, dataprefix)) return j - 1;
  }
#endif
}

// --------------------------------------------------------------------------------
/*!
 * \brief Extract the origin time of the data
 *
 * The information is stored in top level fields:
 *
 *  what.date in YYYYMMDD format
 *  what.time in HHmmss format
 *
 * The implementation sucks by extracting all attributes instead
 * of just the desired ones because the HDF API sucks. Also, we
 * ignore the seconds part of the time field.
 *
 */
// ----------------------------------------------------------------------

boost::posix_time::ptime extract_origin_time(const hid_t &hid)
{
  std::string strdate = get_attribute_value<std::string>(hid, "/what", "date");
  std::string strtime = get_attribute_value<std::string>(hid, "/what", "time");
  std::string stamp = (strdate + strtime).substr(0, 12);

  boost::posix_time::ptime t = Fmi::TimeParser::parse(stamp);

  return t;
}

// ----------------------------------------------------------------------
/*!
 * \brief Extract valid time
 */
// ----------------------------------------------------------------------

boost::posix_time::ptime extract_valid_time(const hid_t &hid, int i)
{
  std::string name = dataset(i) + "/what", strdate, strtime;
  try
  {
    strdate = get_attribute_value<std::string>(hid, name, "enddate");
  }
  catch (std::runtime_error &e)
  {
    name = "/what";
    strdate = get_attribute_value<std::string>(hid, name, "date");
  }

  try
  {
    strtime = get_attribute_value<std::string>(hid, name, "endtime");
  }
  catch (std::runtime_error &e)
  {
    name = "/what";
    strtime = get_attribute_value<std::string>(hid, name, "time");
  }

  std::string stamp = (strdate + strtime).substr(0, 12);

  boost::posix_time::ptime t = Fmi::TimeParser::parse(stamp);

  return t;
}

// ----------------------------------------------------------------------
/*!
 * \brief Create time descriptor for the HDF data
 */
// ----------------------------------------------------------------------

NFmiTimeDescriptor create_tdesc(const hid_t &hid)
{
  boost::posix_time::ptime t = extract_origin_time(hid);
  const NFmiMetTime origintime = tomettime(t);

  const int n = count_datasets(hid);

  NFmiTimeList tlist;

  if (n > 0)
  {
    // Valid dataset specs
    for (int i = 1; i <= n; i++)
    {
      t = extract_valid_time(hid, i);
      tlist.Add(new NFmiMetTime(tomettime(t)));
    }
  }
  else
  {
    // Incorrect specs, we make best guess

    tlist.Add(new NFmiMetTime(origintime));
  }

  return NFmiTimeDescriptor(origintime, tlist);
}

// ----------------------------------------------------------------------
/*!
 * \brief Convert an Opera style parameter name into a newbase name
 *
 * Known instances from Latvia:
 *
 *  filename      product   quantity    newbase
 *
 *  *dBZ.cappi*   PCAPPI    TH          Reflectivity a)
 *  *V.cappi*     PCAPPI    VRAD        RadialVelocity
 *  *W.cappi*     PCAPPI    W b)        SpectralWidth
 *  *Height.eht*  ETOP      HGHT        EchoTop
 *  *dBZ.max*     MAX       TH          CorrectedReflectivity<
 *  *dBA.pac*     RR        ACRR        PrecipitationAmount
 *  *dBA.vil*     VIL       ACRR        PrecipitationAmount
 *  *dBZ.ppi*     PPI       TH          Reflectivity a)
 *  *pcappi-dbz*  PCAPPI    DBZ         Reflectivity a)
 *
 *  a) Latvians should probably be using DBZH instead
 *  b) Latvians should probably be using WRAD instead
 */
// ----------------------------------------------------------------------

FmiParameterName opera_name_to_newbase(const std::string &product,
                                       const std::string &quantity,
                                       const hid_t &hid,
                                       const std::string &prefix)
{
  if (product == "PPI" || product == "CAPPI" || product == "PCAPPI")
  {
    if (quantity == "TH")
      return kFmiReflectivity;
    else if (quantity == "DBZ")
      return kFmiReflectivity;
    else if (quantity == "DBZH")
      return kFmiCorrectedReflectivity;
    else if (quantity == "VRAD")
      return kFmiRadialVelocity;
    else if (quantity == "WRAD" || quantity == "W")  // W is used by Latvians
      return kFmiSpectralWidth;
  }
  else if (product == "ETOP")
  {
    if (quantity == "HGHT") return kFmiEchoTop;
  }
  else if (product == "MAX")
  {
    if (quantity == "TH")
      return kFmiReflectivity;
    else if (quantity == "DBZH")
      return kFmiCorrectedReflectivity;
  }
  else if (product == "RR")
  {
    if (quantity == "ACRR") return kFmiPrecipitationAmount;
  }
  else if (product == "VIL")
  {
    if (quantity == "ACRR") return kFmiPrecipitationAmount;
  }
  else if (product == "SCAN")
  {
    if (quantity == "TH")
      return kFmiReflectivity;
    else if (quantity == "DBZH")
      return kFmiCorrectedReflectivity;
    else if (quantity == "VRAD")
      return kFmiRadialVelocity;
    else if (quantity == "WRAD" || quantity == "W")  // W is used by Latvians
      return kFmiSpectralWidth;
    else if (quantity == "ZDR")
      return kFmiDifferentialReflectivity;
    else if (quantity == "KDP")
      return kFmiSpecificDifferentialPhase;
    else if (quantity == "PHIDP")
      return kFmiDifferentialPhase;
    else if (quantity == "SQI")
      return kFmiSignalQualityIndex;
    else if (quantity == "RHOHV")
      return kFmiReflectivityCorrelation;
  }
  else if (product == "COMP")
  {
    if (quantity == "RATE")
      return kFmiPrecipitationRate;
    else if (quantity == "BRDR")
      return kFmiRadarBorder;
    else if (quantity == "TH")
      return kFmiReflectivity;
    else if (quantity == "DBZH")
      return kFmiCorrectedReflectivity;
    else if (quantity == "PROB")
    {
      // RaVaKe parameters
      int limit = get_attribute_value<int>(hid, prefix, "threshold_id");
      switch (limit)
      {
        case 0:
          return kFmiProbabilityOfPrec;
        case 1:
          return kFmiProbabilityOfPrecLimit1;
        case 2:
          return kFmiProbabilityOfPrecLimit2;
        case 3:
          return kFmiProbabilityOfPrecLimit3;
        case 4:
          return kFmiProbabilityOfPrecLimit4;
        case 5:
          return kFmiProbabilityOfPrecLimit5;
        case 6:
          return kFmiProbabilityOfPrecLimit6;
        case 7:
          return kFmiProbabilityOfPrecLimit7;
        case 8:
          return kFmiProbabilityOfPrecLimit8;
        case 9:
          return kFmiProbabilityOfPrecLimit9;
        case 10:
          return kFmiProbabilityOfPrecLimit10;
        default:
          throw std::runtime_error("Unable to handle parameters of type " + product +
                                   " with quantity " + quantity +
                                   " with threshold_id outside range 0-10");
      }
    }
  }
  else if (product == "VP")
  {
  }
  else if (product == "RHI")
  {
  }
  else if (product == "XSEC")
  {
  }
  else if (product == "VSP")
  {
  }
  else if (product == "HSP")
  {
  }
  else if (product == "RAY")
  {
  }
  else if (product == "AZIM")
  {
  }
  else if (product == "QUAL")
  {
  }

  throw std::runtime_error("Unable to handle parameters of type " + product + " with quantity " +
                           quantity);
}

// ----------------------------------------------------------------------
/*!
 * \brief Create a parameter descriptor
 */
// ----------------------------------------------------------------------

NFmiParamDescriptor create_pdesc(const hid_t &hid)
{
  // First collect the names

  std::set<FmiParameterName> params;
  std::string product, quantity;

  const int n = count_datasets(hid);

  for (int i = 1; i <= n; i++)
  {
    int nj = count_datas(hid, i);

    if (nj > 0)
    {
      // Valid opera data
      for (int j = 1; j <= nj; j++)
      {
        std::string prefix = dataset(i) + "/data" + boost::lexical_cast<std::string>(j);

        product = get_attribute<std::string>(hid, prefix, "what", "product");

        quantity = get_attribute<std::string>(hid, prefix, "what", "quantity");

        FmiParameterName id = opera_name_to_newbase(product, quantity, hid, prefix + "/what");

        if (options.verbose)
          std::cout << "Product: " << product << " Quantity: " << quantity
                    << " Newbase: " << converter.ToString(id) << std::endl;

        params.insert(id);
      }
    }

    else
    {
      // Invalid opera data without data1

      product = get_attribute<std::string>(hid, "/dataset1", "what", "product");

      quantity = get_attribute<std::string>(hid, "/dataset1", "what", "quantity");

      FmiParameterName id =
          opera_name_to_newbase(product, quantity, hid, "/data");  // prefix ok????

      if (options.verbose)
        std::cout << "Product: " << product << " Quantity: " << quantity
                  << " Newbase: " << converter.ToString(id) << std::endl;

      params.insert(id);
    }
  }

  // Then build a parameter bag out of them

  NFmiParamBag pbag;

  BOOST_FOREACH (FmiParameterName id, params)
  {
    std::string name = converter.ToString(id);
    NFmiParam p(id, name);
    p.InterpolationMethod(kLinearly);
    pbag.Add(NFmiDataIdent(p));
  }

  return NFmiParamDescriptor(pbag);
}

// ----------------------------------------------------------------------
/*!
 * \brief Test whether the product has an associated level
 */
// ----------------------------------------------------------------------

bool is_level_parameter(const std::string &product)
{
  if (product == "CAPPI") return true;
  if (product == "PCAPPI") return true;
  if (product == "PPI") return true;
  if (product == "ETOP") return true;
  if (product == "RHI") return true;

  // We cannot extract 2 level values, so we just ignore them
  // if(product == "VIL")   return true;

  return false;
}

// ----------------------------------------------------------------------
/*!
 * \brief Return the level type for the product
 */
// ----------------------------------------------------------------------

FmiLevelType level_type(const std::string &product)
{
  if (product == "CAPPI") return kFmiHeight;
  if (product == "PCAPPI") return kFmiHeight;
  if (product == "PPI") return kFmiAnyLevelType;   // newbase has no angle level
  if (product == "ETOP") return kFmiAnyLevelType;  // newbase has no dBZ level
  if (product == "RHI") return kFmiAnyLevelType;   // newbase has no azimuth levels

  // We cannot extract 2 level values, so we just ignore them
  // if(product == "VIL")   return kFmiHeight;

  return kFmiAnyLevelType;
}

// ----------------------------------------------------------------------
/*!
 * \brief Collect the unique levels in the data
 */
// ----------------------------------------------------------------------

NFmiVPlaceDescriptor collect_levels(const hid_t &hid)
{
  // Check that there is only one level type product

  const int n = count_datasets(hid);

  std::string commonproduct;
  bool haslevels = false;
  bool hasnonlevels = false;

  for (int i = 1; i <= n; i++)
  {
    std::string product = get_attribute_value<std::string>(hid, dataset(i) + "/what", "product");

    bool islevel = is_level_parameter(product);

    if (islevel)
      haslevels = true;
    else
      hasnonlevels = true;

    if (commonproduct.empty())
    {
      commonproduct = product;
    }
    else if (islevel && commonproduct != product)
      throw std::runtime_error("Cannot have different kinds of products when level data is used: " +
                               commonproduct + " and " + product);
  }

  if (hasnonlevels && haslevels)
    throw std::runtime_error("Cannot mix non-level type parameters with level type parameters");

  if (hasnonlevels) return NFmiVPlaceDescriptor();

  // Now we need to collect the unique level values

  std::set<double> levels;

  if (options.verbose) std::cout << "Level values:" << std::endl;

  for (int i = 1; i <= n; i++)
  {
    double prodpar = get_attribute_value<double>(hid, dataset(i) + "/what", "prodpar");
    if (options.verbose) std::cout << "  " << i << ": " << prodpar << std::endl;
    levels.insert(prodpar);
  }

  // And finally build the level bag

  FmiLevelType ltype = level_type(commonproduct);

  NFmiLevelBag lbag;

  BOOST_FOREACH (double lvalue, levels)
  {
    NFmiLevel l(ltype, commonproduct, lvalue);
    if (options.verbose) std::cout << commonproduct << " level value: " << lvalue << std::endl;
    lbag.AddLevel(l);
  }
  return NFmiVPlaceDescriptor(lbag);
}

// ----------------------------------------------------------------------
/*!
 * \brief Collect the unique PVOL elevation angles in the data
 */
// ----------------------------------------------------------------------

NFmiVPlaceDescriptor collect_pvol_levels(const hid_t &hid)
{
  // Check that there is only one level type product

  const int n = count_datasets(hid);

  // Now we need to collect the unique elevation angles

  std::set<double> angles;

  if (options.verbose) std::cout << "Elevation angles:" << std::endl;

  for (int i = 1; i <= n; i++)
  {
    double angle = get_attribute_value<double>(hid, dataset(i) + "/where", "elangle");
    if (options.verbose) std::cout << "  " << i << ": " << angle << std::endl;
    angles.insert(angle);
  }

  // And finally build the level bag

  FmiLevelType ltype = kFmiNoLevelType;  // newbase does not define a SCAN level

  NFmiLevelBag lbag;

  BOOST_FOREACH (double angle, angles)
  {
    std::string levelname = "Elevation angle " + boost::lexical_cast<std::string>(angle);
    NFmiLevel l(ltype, levelname, angle);
    lbag.AddLevel(l);
  }
  return NFmiVPlaceDescriptor(lbag);
}

// ----------------------------------------------------------------------
/*!
 * \brief Create a vertical descriptor
 *
 * The following products have an associated prodpar which describes
 * the level in some manner:
 *
 *   CAPPI  Layer height in meters above the radar
 *   PCAPPI Layer height in meters above the radar
 *   PPI    Elevation angle in degrees
 *   ETOP   Reflectivity limit in dBZ (clouds=-10, rain=10, thunder=20 etc)
 *   RHI    Azimuth angle in degrees
 *   VIL    Bottom and top heights of the integration layer
 *
 * PVOL data has a where/elangle attribute which is used as the level value.
 *
 * We ignore the VIL level values
 */
// ----------------------------------------------------------------------

NFmiVPlaceDescriptor create_vdesc(const hid_t &hid)
{
  std::string object = get_attribute_value<std::string>(hid, "/what", "object");

  if (object == "COMP")
  {
    return collect_levels(hid);
  }
  else if (object == "PVOL")
  {
    return collect_pvol_levels(hid);
  }
  else if (object == "CVOL")
  {
    return collect_levels(hid);
  }
  else if (object == "SCAN")
  {
    return collect_levels(hid);
  }
  else if (object == "RAY")
  {
    throw std::runtime_error("This program cannot handle single polar rays (RAY)  data");
  }
  else if (object == "AZIM")
  {
    throw std::runtime_error("This program cannot handle azimuthal objects (AZIM)  data");
  }
  else if (object == "IMAGE")
  {
    return collect_levels(hid);
  }
  else if (object == "XSEC")
  {
    throw std::runtime_error("This program cannot handle 2D vertical cross sections (XSEC) data");
  }
  else if (object == "VP")
  {
    throw std::runtime_error("This program cannot handle vertical profile (VP) data");
  }
  else if (object == "PIC")
  {
    throw std::runtime_error("This program cannot handle embedded graphical image (PIC) data");
  }
  else
    throw std::runtime_error("Unknown data object: '" + object +
                             "' is not listed in the Opera specs followed by this implementation");
}

// ----------------------------------------------------------------------
/*!
 * \brief Calculate PVOL range
 *
 * Each dataset has the following attributes:
 *
 * - elangle, the elevation angle of the scan
 * - nbins, the number of bins in a ray, f.ex 500
 * - rstart, the starting offset in kilometers for bin 1
 * - rscale, the distance in meters between bins
 */
// ----------------------------------------------------------------------

double calculate_pvol_range(const hid_t &hid)
{
  int n = count_datasets(hid);

  double maxrange = -1;

  const double pi = 3.14159265358979326;

  for (int i = 1; i <= n; i++)
  {
    std::string prefix = dataset(i) + "/where";

    double elangle = get_attribute_value<double>(hid, prefix, "elangle");
    double nbins = get_attribute_value<double>(hid, prefix, "nbins");
    double rstart = get_attribute_value<double>(hid, prefix, "rstart");
    double rscale = get_attribute_value<double>(hid, prefix, "rscale");

    double range = 1000 * rstart + nbins * rscale * cos(elangle * pi / 180);
    maxrange = std::max(maxrange, range);
  }

  return maxrange;
}

// ----------------------------------------------------------------------
/*!
 * \brief Calculate nbins
 */
// ----------------------------------------------------------------------

int calculate_nbins(const hid_t &hid)
{
  int n = count_datasets(hid);

  int nbins = -1;

  for (int i = 1; i <= n; i++)
  {
    std::string prefix = dataset(i) + "/where";
    int tmp = get_attribute_value<int>(hid, prefix, "nbins");

    nbins = std::max(nbins, tmp);
  }

  return nbins;
}

// ----------------------------------------------------------------------
/*!
 * \brief Create horizontal place descriptor
 *
 */
// ----------------------------------------------------------------------

NFmiHPlaceDescriptor create_hdesc(const hid_t &hid)
{
  std::string object = get_attribute_value<std::string>(hid, "/what", "object");

  const NFmiPoint xy0(0, 0);
  const NFmiPoint xy1(1, 1);

  if (object == "COMP" || object == "IMAGE" || object == "CVOL")
  {
    std::string projdef = get_attribute_value<std::string>(hid, "/where", "projdef");
    long xsize = get_attribute_value<long>(hid, "/where", "xsize");
    long ysize = get_attribute_value<long>(hid, "/where", "ysize");

    // Latvian style corners
    if (!(is_group_attribute<double>(hid, "/where", "LL_lon")))
    {
      double LR_lon = get_attribute_value<double>(hid, "/where", "LR_lon");
      double LR_lat = get_attribute_value<double>(hid, "/where", "LR_lat");
      double UL_lon = get_attribute_value<double>(hid, "/where", "UL_lon");
      double UL_lat = get_attribute_value<double>(hid, "/where", "UL_lat");
      boost::shared_ptr<NFmiArea> tmparea = NFmiAreaFactory::CreateProj(
          projdef, NFmiPoint(UL_lon, LR_lat), NFmiPoint(LR_lon, UL_lat));

      // Convert real corners to world xy

      NFmiPoint ul = tmparea->LatLonToWorldXY(NFmiPoint(UL_lon, UL_lat));
      NFmiPoint lr = tmparea->LatLonToWorldXY(NFmiPoint(LR_lon, LR_lat));

      // Switched corners

      NFmiPoint ll = NFmiPoint(ul.X(), lr.Y());
      NFmiPoint ur = NFmiPoint(lr.X(), ul.Y());

      // Back to lat lon

      NFmiPoint LL = tmparea->WorldXYToLatLon(ll);
      NFmiPoint UR = tmparea->WorldXYToLatLon(ur);

      boost::shared_ptr<NFmiArea> area = NFmiAreaFactory::CreateProj(projdef, LL, UR);
      NFmiGrid grid(area->Clone(), xsize, ysize);
      return NFmiHPlaceDescriptor(grid);
    }

    // FMI style corners
    else
    {
      double LL_lon = get_attribute_value<double>(hid, "/where", "LL_lon");
      double LL_lat = get_attribute_value<double>(hid, "/where", "LL_lat");
      double UR_lon = get_attribute_value<double>(hid, "/where", "UR_lon");
      double UR_lat = get_attribute_value<double>(hid, "/where", "UR_lat");

      boost::shared_ptr<NFmiArea> area = NFmiAreaFactory::CreateProj(
          projdef, NFmiPoint(LL_lon, LL_lat), NFmiPoint(UR_lon, UR_lat));

      NFmiGrid grid(area->Clone(), xsize, ysize);
      return NFmiHPlaceDescriptor(grid);
    }
  }

  else if (object == "PVOL")
  {
    const double lon = get_attribute_value<double>(hid, "/where", "lon");
    const double lat = get_attribute_value<double>(hid, "/where", "lat");
    // const double height = get_attribute_value<double>(hid,"/where","height");

    // Max range in meters and then rounded up to kilometers

    const double range_m = calculate_pvol_range(hid);
    const double range_km = std::ceil(range_m / 1000);

    NFmiArea *area = new NFmiEquidistArea(1000 * range_km, NFmiPoint(lon, lat), xy0, xy1);

    // We set the grid resolution based on the number of bins in the data

    const int nbins = calculate_nbins(hid);
    NFmiGrid grid(area, 2 * nbins, 2 * nbins);

    return NFmiHPlaceDescriptor(grid);
  }
  else if (object == "SCAN")
  {
#if 0
	  const double lon    = get_attribute_value<double>(hid,"/where","lon");
	  const double lat    = get_attribute_value<double>(hid,"/where","lat");
	  const double height = get_attribute_value<double>(hid,"/where","height");
#endif

    throw std::runtime_error("This program cannot handle " + object + " data");
  }
  else if (object == "RAY" || object == "AZIM" || object == "XSEC" || object == "VP" ||
           object == "PIC")
  {
    throw std::runtime_error("This program cannot handle where-information of " + object + " data");
  }
  else
    throw std::runtime_error("Unknown data object: '" + object +
                             "' is not listed in the Opera specs followed by this implementation");
}

// ----------------------------------------------------------------------
/*!
 * \brief Print information on group attributes
 */
// ----------------------------------------------------------------------

void print_group_attributes(const hid_t &hid, const std::string &dpath)
{
  if (!H5Utilities::isGroup(hid, dpath)) return;

  hid_t gid = H5Utilities::openHDF5Object(hid, dpath);
  if (!gid) throw std::runtime_error("Failed to open " + dpath);
  std::cout << "Opened " << dpath << std::endl;

  MXAAbstractAttributes attrs;
  if (!H5Utilities::readAllAttributes(hid, dpath, attrs))
    throw std::runtime_error("Failed to read " + dpath + " attributes");

  BOOST_FOREACH (const MXAAbstractAttributes::value_type &name_ptr, attrs)
  {
    std::cout << "Attribute: " << dpath << "/" << name_ptr.first << " ( "
              << H5Lite::StringForHDFType(name_ptr.second->getDataType())
              << " ) = " << get_attribute_string(name_ptr.first, *name_ptr.second) << std::endl;
  }

  H5Utilities::closeHDF5Object(gid);
}

// ----------------------------------------------------------------------
/*!
 * \brief Print information on the HDF file
 */
// ----------------------------------------------------------------------

void print_hdf_information(const hid_t &hid)
{
  const int n = count_datasets(hid);
  std::cout << "Number of datasets: " << n << std::endl;

  // Note that isGroup and the underlying HDF5 library call
  // will dump error messages unless you are using a patched
  // version of MXA Datamodel. The patch is documented in our
  // wiki, look for MXA.

  print_group_attributes(hid, "/what");
  print_group_attributes(hid, "/where");
  print_group_attributes(hid, "/how");

  // print_group_attributes(hid,"/dataset1/data1/what");

  for (int i = 1; i <= n; i++)
  {
    std::string prefix = dataset(i) + "/";
    print_group_attributes(hid, prefix + "what");
    print_group_attributes(hid, prefix + "where");
    print_group_attributes(hid, prefix + "how");

    // Print more detailed information for each parameter in each dataset

    int nj = count_datas(hid, i);
    for (int j = 1; j <= nj; j++)
    {
      std::string dataprefix = prefix + "data" + boost::lexical_cast<std::string>(j) + "/";

      print_group_attributes(hid, dataprefix + "what");
      print_group_attributes(hid, dataprefix + "where");
      print_group_attributes(hid, dataprefix + "how");
    }
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Apply gain and offset
 */
// ----------------------------------------------------------------------

double apply_gain_offset(double value,
                         const boost::optional<double> &gain,
                         const boost::optional<double> &offset)
{
  if (gain) value *= *gain;
  if (offset) value += *offset;
  return value;
}

// ----------------------------------------------------------------------
/*!
 * \brief Copy one dataset
 */
// ----------------------------------------------------------------------

void copy_dataset(const hid_t &hid, NFmiFastQueryInfo &info, int datanum)
{
  std::string prefix = options.datasetname + boost::lexical_cast<std::string>(datanum);

  // Set level

  info.FirstLevel();  // default

  int n = count_datas(hid, datanum);

  if (n > 0)
  {
    // Valid Opera data
    for (int i = 1; i <= n; i++)
    {
      std::string iprefix = ("/" + prefix + "/data" + boost::lexical_cast<std::string>(i));

      // Establish product details
      std::string product = get_attribute<std::string>(hid, iprefix, "what", "product");
      std::string quantity = get_attribute<std::string>(hid, iprefix, "what", "quantity");

      if (is_level_parameter(product))
      {
        double prodpar = get_attribute<double>(hid, iprefix, "what", "prodpar");
        NFmiLevel level(level_type(product), product, prodpar);
        if (!info.Level(level))
          throw std::runtime_error("Failed to activate correct level in output querydata");
      }

      // Establish numeric transformation

      auto nodata = get_optional_double(hid, iprefix, "what", "nodata");
      auto undetect = get_optional_double(hid, iprefix, "what", "undetect");
      auto gain = get_optional_double(hid, iprefix, "what", "gain");
      auto offset = get_optional_double(hid, iprefix, "what", "offset");

      FmiParameterName id = opera_name_to_newbase(product, quantity, hid, iprefix + "/what");

      if (!info.Param(id))
        throw std::runtime_error("Failed to activate product " + product +
                                 " in output querydata with id " + converter.ToString(id));

      boost::posix_time::ptime t = extract_valid_time(hid, datanum);
      if (!info.Time(tomettime(t)))
        throw std::runtime_error("Failed to activate correct valid time in output querydata");

      if (options.verbose)
        std::cout << "Copying dataset " << datanum << " part " << i << " with valid time " << t
                  << std::endl;

      std::vector<int> values;

      if (options.verbose) std::cout << "Reading " << iprefix << "/data" << std::endl;

      if (H5Lite::readVectorDataset(hid, iprefix + "/data", values) != 0)
        throw std::runtime_error("Failed to read " + iprefix + "/data");

      const unsigned long width = info.Grid()->XNumber();
      const unsigned long height = info.Grid()->YNumber();

      unsigned long pos = 0;

      for (info.ResetLocation(); info.NextLocation();)
      {
        // Newbase grid location
        unsigned long i = pos % width;
        unsigned long j = pos / width;
        // HDF5 grid location is inverted
        j = height - j - 1;
        // Respective vector position
        unsigned long k = i + width * j;

        // And copy the value
        int value = values[k];

        if (nodata && value == *nodata)
          info.FloatValue(kFloatMissing);
        else if (undetect && value == *undetect)
          info.FloatValue(apply_gain_offset(0, gain, offset));
        else
          info.FloatValue(apply_gain_offset(value, gain, offset));

        ++pos;
      }
    }
  }
  else
  {
    // Unnumbered data used in Latvia

    // Establish product details
    std::string product = get_attribute<std::string>(hid, prefix, "what", "product");
    std::string quantity = get_attribute<std::string>(hid, prefix, "what", "quantity");

    if (is_level_parameter(product))
    {
      double prodpar = get_attribute<double>(hid, prefix, "what", "prodpar");
      NFmiLevel level(level_type(product), product, prodpar);
      if (!info.Level(level))
        throw std::runtime_error("Failed to activate correct level in output querydata");
    }

    // Establish numeric transformation

    auto nodata = get_optional_double(hid, prefix, "what", "nodata");
    auto undetect = get_optional_double(hid, prefix, "what", "undetect");
    auto gain = get_optional_double(hid, prefix, "what", "gain");
    auto offset = get_optional_double(hid, prefix, "what", "offset");

    FmiParameterName id = opera_name_to_newbase(product, quantity, hid, "/" + prefix + "/what");

    if (!info.Param(id))
      throw std::runtime_error("Failed to activate product " + product +
                               " in output querydata with id " + converter.ToString(id));
// Copy the values

#if 0
	  // Crashes in RHEL6
	  int32_t htype;
	  H5Utilities::getObjectType(hid,prefix+"/data",&htype);
	  
	  if(options.verbose)
		std::cout << "Reading " << prefix+"/data of type " << H5Lite::StringForHDFType(htype) << std::endl;
#endif

    std::vector<int> values;

    if (options.verbose) std::cout << "Reading " << prefix + "/data" << std::endl;

    if (H5Lite::readVectorDataset(hid, prefix + "/data", values) != 0)
      throw std::runtime_error("Failed to read " + prefix + "/data");

// Copy values into querydata. Unfortunately a simple loop
// will not do, the data would go upside down. Hence we need
// to index the data directly.

#if 1
    const unsigned long width = info.Grid()->XNumber();
    const unsigned long height = info.Grid()->YNumber();

    unsigned long pos = 0;

    for (info.ResetLocation(); info.NextLocation();)
    {
      // Newbase grid location
      unsigned long i = pos % width;
      unsigned long j = pos / width;
      // HDF5 grid location is inverted
      j = height - j - 1;
      // Respective vector position
      unsigned long k = i + width * j;

      // And copy the value
      int value = values[k];

      if (nodata && value == *nodata)
        info.FloatValue(kFloatMissing);
      else if (undetect && value == *undetect)
        info.FloatValue(apply_gain_offset(0, gain, offset));
      else
        info.FloatValue(apply_gain_offset(value, gain, offset));

      ++pos;
    }
#else
    // Does not work
    info.ResetLocation();

    BOOST_FOREACH (int value, values)
    {
      info.NextLocation();
      if (nodata && value == *nodata)
        info.FloatValue(kFloatMissing);
      else if (undetect && value == *undetect)
        info.FloatValue(apply_gain_offset(0, gain, offset));
      else
        info.FloatValue(apply_gain_offset(value, gain, offset));
    }
#endif
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Copy one PVOL dataset
 */
// ----------------------------------------------------------------------

void copy_dataset_pvol(const hid_t &hid, NFmiFastQueryInfo &info, int datanum)
{
  std::string prefix = options.datasetname + boost::lexical_cast<std::string>(datanum);

  // Set time

  boost::posix_time::ptime t = extract_valid_time(hid, 1);
  if (!info.Time(tomettime(t)))
    throw std::runtime_error("Failed to activate correct valid time in output querydata");

  // Set parameter

  std::string product = get_attribute_value<std::string>(hid, prefix + "/what", "product");
  std::string quantity = get_attribute_value<std::string>(hid, prefix + "/data1/what", "quantity");

  FmiParameterName id = opera_name_to_newbase(product, quantity, hid, prefix);

  if (!info.Param(id))
    throw std::runtime_error("Failed to activate product " + product + " in output querydata");

  // Set level

  info.ResetLevel();
  for (int level = 0; level < datanum; level++)
    info.NextLevel();

  // Establish numeric transformation

  auto nodata = get_optional_double(hid, prefix + "/data1/what", "nodata");
  auto undetect = get_optional_double(hid, prefix + "/data1/what", "undetect");
  auto gain = get_optional_double(hid, prefix + "/data1/what", "gain");
  auto offset = get_optional_double(hid, prefix + "/data1/what", "offset");

  // Establish measurement details

  double lat = get_attribute_value<double>(hid, "/where", "lat");
  double lon = get_attribute_value<double>(hid, "/where", "lon");

  // int a1gate     = get_attribute_value<int>(hid,prefix+"/where","a1gate");
  double elangle = get_attribute_value<double>(hid, prefix + "/where", "elangle");
  int nbins = get_attribute_value<int>(hid, prefix + "/where", "nbins");
  int nrays = get_attribute_value<int>(hid, prefix + "/where", "nrays");
  double rscale = get_attribute_value<double>(hid, prefix + "/where", "rscale");
  double rstart = get_attribute_value<double>(hid, prefix + "/where", "rstart");

// Copy the values

#if 0
  // Crashes in RHEL6
  int32_t htype;
  H5Utilities::getObjectType(hid,prefix+"/data1/data",&htype);

  if(options.verbose)
	std::cout << "Reading " << prefix+"/data1/data of type " << H5Lite::StringForHDFType(htype) << std::endl;
#else
  if (options.verbose) std::cout << "Reading " << prefix + "/data1/data" << std::endl;
#endif

  std::vector<int> values;

  if (H5Lite::readVectorDataset(hid, prefix + "/data1/data", values) != 0)
    throw std::runtime_error("Failed to read " + prefix + "/data");

  // Center location in meters

  NFmiPoint center = info.Area()->LatLonToWorldXY(NFmiPoint(lon, lat));

  // Copy values into querydata. See section 5.1 of the Opera specs for details.
  // According to it we can ignore a1gate for polar volumes

  const double pi = 3.14159265358979323;

  for (int ray = 0; ray < nrays; ray++)
  {
    // Angle of the ray in degrees and then in radians.
    // 0.5 is added since the first scan represents angle starting from 0,
    // not centered around it

    double angle = 360 * (ray + 0.5) / nrays;
    double alpha = angle * pi / 180;

    for (int bin = 0; bin < nbins; ++bin)
    {
      // Distance along the ray, taking elevation into account
      // 0.5 moves us into the center of the bin
      double r = (1000 * rstart + (bin + 0.5) * rscale) * cos(elangle * pi / 180);

      // Respective world XY coordinate
      NFmiPoint p(center.X() + r * sin(alpha), center.Y() + r * cos(alpha));

      // And latlon
      NFmiPoint latlon = info.Area()->WorldXYToLatLon(p);

      if (info.NearestPoint(latlon))
      {
        // And copy the value
        int value = values[ray * nbins + bin];

        if (nodata && value == *nodata)
          info.FloatValue(kFloatMissing);
        else if (undetect && value == *undetect)
          info.FloatValue(apply_gain_offset(0, gain, offset));
        else
          info.FloatValue(apply_gain_offset(value, gain, offset));
      }
      else
        std::runtime_error("Internal error when projecting PVOL data to cartesian coordinates");
    }
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Copy HDF values into querydata
 *
 * We iterate through all the datasets, find the time, param etc info,
 * activate it in the info object, and copy the grid.
 */
// ----------------------------------------------------------------------

void copy_datasets(const hid_t &hid, NFmiFastQueryInfo &info)
{
  std::string obj = get_attribute_value<std::string>(hid, "/what", "object");

  const int n = count_datasets(hid);
  for (int i = 1; i <= n; i++)
  {
    if (obj == "PVOL")
      copy_dataset_pvol(hid, info, i);
    else
      copy_dataset(hid, info, i);
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Main program without exception handling
 */
// ----------------------------------------------------------------------

int run(int argc, char *argv[])
{
  if (!parse_options(argc, argv)) return 0;

  if (options.verbose) std::cout << "Opening file '" << options.infile << "'" << std::endl;

  hid_t hid = H5Utilities::openFile(options.infile, true);  // true = read only
  if (hid < 0) throw std::runtime_error("Failed to open '" + options.infile + "' for reading");

  // Check that the data looks like Opera radar HDF data

  validate_hdf(hid);

  // Print information on the data in verbose mode

  if (options.verbose) print_hdf_information(hid);

  // Create the output projection if there is one. We do it before doing any
  // work so that the user gets a fast response to a possible syntax error

  boost::shared_ptr<NFmiArea> area;
  if (!options.projection.empty()) area = NFmiAreaFactory::Create(options.projection);

  // Create query data descriptors

  NFmiTimeDescriptor tdesc = create_tdesc(hid);
  NFmiParamDescriptor pdesc = create_pdesc(hid);
  NFmiVPlaceDescriptor vdesc = create_vdesc(hid);
  NFmiHPlaceDescriptor hdesc = create_hdesc(hid);

  NFmiFastQueryInfo qi(pdesc, tdesc, hdesc, vdesc);
  boost::shared_ptr<NFmiQueryData> data(NFmiQueryDataUtil::CreateEmptyData(qi));
  NFmiFastQueryInfo info(data.get());

  if (data.get() == 0) throw std::runtime_error("Could not allocate memory for result data");

  info.SetProducer(NFmiProducer(options.producernumber, options.producername));

  copy_datasets(hid, info);

  // Reproject if so requested

  if (area)
  {
    int width = static_cast<int>(round(area->XYArea(area.get()).Width()));
    int height = static_cast<int>(round(area->XYArea(area.get()).Height()));

    NFmiGrid grid(area.get(), width, height);
    boost::shared_ptr<NFmiQueryData> tmp(
        NFmiQueryDataUtil::Interpolate2OtherGrid(data.get(), &grid, NULL));
    std::swap(data, tmp);
  }

  if (options.outfile == "-")
    std::cout << *data;
  else
  {
    std::ofstream out(options.outfile.c_str());
    out << *data;
  }

  return 0;
}

// ----------------------------------------------------------------------
/*!
 * \brief Main program
 */
// ----------------------------------------------------------------------

int main(int argc, char *argv[])
{
  try
  {
    return run(argc, argv);
  }
  catch (std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cerr << "Error: Caught an unknown exception" << std::endl;
    return 1;
  }
}
