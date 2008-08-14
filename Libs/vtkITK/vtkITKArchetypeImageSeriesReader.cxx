/*=========================================================================

  Copyright Brigham and Women's Hospital (BWH) All Rights Reserved.

  See Doc/copyright/copyright.txt
  or http://www.slicer.org/copyright/copyright.txt for details.

  Program:   vtkITK
  Module:    $HeadURL$
  Date:      $Date$
  Version:   $Revision$

==========================================================================*/
#include "vtkITKArchetypeImageSeriesReader.h"

#include "vtkImageData.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkDataArray.h"

#include "itkExceptionObject.h"
#include "itkTimeProbe.h"

// Commented out redefinition of ExceptionMacro
#ifdef REDEFINE_EXCEPTION_MACROS
// turn itk exceptions into vtk errors
#undef itkExceptionMacro  
#define itkExceptionMacro(x) \
  { \
  ::itk::OStringStream message; \
  message << "itk::ERROR: " << this->GetNameOfClass() \
          << "(" << this << "): " x; \
  std::cout << message.str().c_str() << std::endl; \
  }

#undef itkGenericExceptionMacro  
#define itkGenericExceptionMacro(x) \
  { \
  ::itk::OStringStream message; \
  message << "itk::ERROR: " x; \
  std::cout << message.str().c_str() << std::endl; \
  }
#endif


//
// uncomment the define below to enable use of the GE5 (Signa) reader
// this is not on by default because the reader does not support
// reading directions from the file.
// The GE5 reader was fixed just after the itk 3.2 release
//
#if (ITK_VERSION_MAJOR > 3) || \
((ITK_VERSION_MAJOR == 3 && ITK_VERSION_MINOR >= 2))
#define USE_ITKGE5READER
#endif

#ifdef USE_ITKGE5READER
#include "itkImageIOFactory.h"
#include "itkMutexLock.h"
#include "itkMutexLockHolder.h"
#include "itkGE5ImageIOFactory.h"
#endif


#include "itkArchetypeSeriesFileNames.h"
#include "itkImage.h"
#include "itkVector.h"
#include "itkOrientImageFilter.h"
#include "itkImageSeriesReader.h"
#include "itkImageFileReader.h"
#include "itkImportImageContainer.h"
#include "itkImageRegion.h"
#include "itkGDCMSeriesFileNames.h"
#include "itkGDCMImageIO.h"
#include "itkMetaImageIO.h"
#include "itkNrrdImageIO.h"
#include "itkGE5ImageIO.h"
#include "itkBrains2MaskImageIOFactory.h"
#include "itkBrains2MaskImageIO.h"
#include "itkNiftiImageIO.h"
#include "itkVTKImageIO.h"
#include "itkTIFFImageIO.h"
#include "itkAnalyzeImageIO.h"
#include <itksys/SystemTools.hxx>

vtkCxxRevisionMacro(vtkITKArchetypeImageSeriesReader, "$Revision$");
vtkStandardNewMacro(vtkITKArchetypeImageSeriesReader);

//----------------------------------------------------------------------------
vtkITKArchetypeImageSeriesReader::vtkITKArchetypeImageSeriesReader()
{
  this->Archetype  = NULL;
  this->SingleFile = 1;
  this->UseOrientationFromFile = 1;
  this->RasToIjkMatrix = NULL;
  this->SetDesiredCoordinateOrientationToAxial();
  this->UseNativeCoordinateOrientation = 0;
  this->FileNameSliceOffset = 0;
  this->FileNameSliceSpacing = 1;
  this->FileNameSliceCount = 0;
  this->UseNativeOrigin = false;
  this->OutputScalarType = VTK_FLOAT;
  this->NumberOfComponents = 0;
  this->UseNativeScalarType = 0;
  for (int i = 0; i < 3; i++)
    {
    this->DefaultDataSpacing[i] = 1.0;
    this->DefaultDataOrigin[i] = 0.0;
    }

  this->SeriesInstanceUIDs.resize( 0 );
  this->ContentTime.resize( 0 );
  this->TriggerTime.resize( 0 );
  this->DiffusionGradientOrientation.resize( 0 );
  this->SliceLocation.resize( 0 );
  this->ImageOrientationPatient.resize( 0 );

  this->AnalyzeHeader = true;

  this->GroupingByTags = false;
  this->IsOnlyFile = false;

  this->SelectedUID = -1;
  this->SelectedContentTime = -1;
  this->SelectedTriggerTime = -1;
  this->SelectedDiffusion = -1;
  this->SelectedSlice = -1;
  this->SelectedOrientation = -1;

  this->RegisterExtraBuiltInFactories();
}

// 
// ITK internally does not register all of the IO types that get built
// (possibly due to lingering bugs?) but many slicer users have
// GE5 (Signa - magic number: IMGF) files that they need to work
// with so we register the factory explictly here
//
void
vtkITKArchetypeImageSeriesReader::RegisterExtraBuiltInFactories()
{
#ifdef USE_ITKGE5READER
  static bool firstTime = true;

  static itk::SimpleMutexLock mutex;
  {
  // This helper class makes sure the Mutex is unlocked
  // in the event an exception is thrown.
  itk::MutexLockHolder<itk::SimpleMutexLock> mutexHolder( mutex );
  if( firstTime )
    {
    itk::ObjectFactoryBase::RegisterFactory( itk::Brains2MaskImageIOFactory::New() );
    itk::ObjectFactoryBase::RegisterFactory( itk::GE5ImageIOFactory::New() );
    firstTime = false;
    }
  }
#endif

}

//----------------------------------------------------------------------------
vtkITKArchetypeImageSeriesReader::~vtkITKArchetypeImageSeriesReader()
{ 
  if (this->Archetype)
    {
    delete [] this->Archetype;
    this->Archetype = NULL;
    }
 if (RasToIjkMatrix)
   {
   RasToIjkMatrix->Delete();
   RasToIjkMatrix = NULL;
   }
  
}

vtkMatrix4x4* vtkITKArchetypeImageSeriesReader::GetRasToIjkMatrix()
{
  this->UpdateInformation();
  return RasToIjkMatrix;
}

//----------------------------------------------------------------------------
void vtkITKArchetypeImageSeriesReader::PrintSelf(ostream& os, vtkIndent indent)
{
  int idx;
  
  this->Superclass::PrintSelf(os,indent);

  os << indent << "Archetype: " <<
    (this->Archetype ? this->Archetype : "(none)") << "\n";

  os << indent << "FileNameSliceOffset: " 
     << this->FileNameSliceOffset << "\n";
  os << indent << "FileNameSliceSpacing: " 
     << this->FileNameSliceSpacing << "\n";
  os << indent << "FileNameSliceCount: " 
     << this->FileNameSliceCount << "\n";

  os << indent << "OutputScalarType: "
     << vtkImageScalarTypeNameMacro(this->OutputScalarType)
     << std::endl;
  os << indent << "DefaultDataSpacing: (" << this->DefaultDataSpacing[0];
  for (idx = 1; idx < 3; ++idx)
    {
    os << ", " << this->DefaultDataSpacing[idx];
    }
  os << ")\n";
  
  os << indent << "DefaultDataOrigin: (" << this->DefaultDataOrigin[0];
  for (idx = 1; idx < 3; ++idx)
    {
    os << ", " << this->DefaultDataOrigin[idx];
    }
  os << ")\n";
  
}

int vtkITKArchetypeImageSeriesReader::CanReadFile(const char* filename)
{
  std::string fileNameCollapsed = itksys::SystemTools::CollapseFullPath( this->Archetype);

  // First see if the archetype exists
  if (!itksys::SystemTools::FileExists (fileNameCollapsed.c_str()))
    {
    vtkDebugMacro(<<"The filename does not exist.");
    return false;
    }
  return true;
}

//----------------------------------------------------------------------------
// This method returns the largest data that can be generated.
void vtkITKArchetypeImageSeriesReader::ExecuteInformation()
{
  vtkImageData *output = this->GetOutput();
  std::vector<std::string> candidateFiles;
  std::vector<std::string> candidateSeries;
  int extent[6];  
  std::string fileNameCollapsed = itksys::SystemTools::CollapseFullPath( this->Archetype);

  if ( this->SingleFile )
  {
    this->AnalyzeHeader = false;
  }

  // Since we only need origin, spacing and extents, we can use one
  // image type.
  typedef itk::Image<float,3> ImageType;
  itk::ImageRegion<3> region;

  typedef itk::ImageSource<ImageType> FilterType;
  FilterType::Pointer filter;

  // First see if the archetype exists, if it's not a pointer into memory
  if (fileNameCollapsed.find("slicer:0x") != std::string::npos &&
      fileNameCollapsed.find("#") != std::string::npos)
    {
    vtkDebugMacro("File " << fileNameCollapsed.c_str() << " is a pointer to the mrml scene in memory, not checking for it on disk");
    }
  else 
    {
    if (!itksys::SystemTools::FileExists (fileNameCollapsed.c_str()))
      {
      itkGenericExceptionMacro ( "vtkITKArchetypeImageSeriesReader::ExecuteInformation: Archetype file " << fileNameCollapsed.c_str() << " does not exist.");
      return;
      }
    }

  this->AllFileNames.resize( 0 );

  // Some file types require special processing
  itk::GDCMImageIO::Pointer dicomIO = itk::GDCMImageIO::New();

  // Test whether the input file is a DICOM file
  bool isDicomFile = dicomIO->CanReadFile(this->Archetype);

  // if user already set up FileNames, we do not try to find candidate files
  if ( this->GetNumberOfFileNames() > 0 )  
  {
    unsigned int nFiles = this->GetNumberOfFileNames();
    this->AllFileNames.resize( 0 );
    for (unsigned int k = 0; k < nFiles; k++)
    {
      this->AllFileNames.push_back( this->FileNames[k] );
    }
    this->FileNames.resize( 0 );
    
    // if this is the only file set by user
    if (nFiles == 1)
    {
      this->IsOnlyFile = true;
    }

    // if we need to analyze the header
    if ( AnalyzeHeader )
    {
      this->AnalyzeDicomHeaders();
    }
  }
  else
  {
    if ( isDicomFile && !this->GetSingleFile() )
    {
      typedef itk::GDCMSeriesFileNames DICOMNameGeneratorType;
      DICOMNameGeneratorType::Pointer inputImageFileGenerator = DICOMNameGeneratorType::New();
      std::string fileNameName = itksys::SystemTools::GetFilenameName( this->Archetype );
      std::string fileNamePath = itksys::SystemTools::GetFilenamePath( this->Archetype );
      if (fileNamePath == "")
      {
        fileNamePath = ".";
      }
      inputImageFileGenerator->SetDirectory( fileNamePath );

      // determine if the file is diffusion weighted MR file

      // Find the series that contains the archetype
      candidateSeries = inputImageFileGenerator->GetSeriesUIDs();

      // Find all dicom files in the directory 
      for (unsigned int s = 0; s < candidateSeries.size(); s++)
      {
        std::vector<std::string> seriesFileNames;
        seriesFileNames = inputImageFileGenerator->GetFileNames( candidateSeries[s] ); 
        for (unsigned int f = 0; f < seriesFileNames.size(); f++)
        {
          this->AllFileNames.push_back( seriesFileNames[f] );
        }
      }
      //int nFiles = this->AllFileNames.size(); UNUSED

      // analysis dicom files and fill the Dicom Tag arrays
      if ( AnalyzeHeader )
      {
        this->AnalyzeDicomHeaders();
      }

      // the following for loop set up candidate files with same series number 
      // that include the given Archetype;
      int found = 0;
      for (unsigned int s = 0; s < candidateSeries.size() && found == 0; s++)
      {
        candidateFiles = inputImageFileGenerator->GetFileNames(candidateSeries[s]);
        for (unsigned int f = 0; f < candidateFiles.size(); f++)
        {
          if (itksys::SystemTools::CollapseFullPath(candidateFiles[f].c_str()) ==
            fileNameCollapsed)
          {
            found = 1;
            break;
          }
        }
      }

      // do we have just one candidate file
      if ( candidateFiles.size() == 1 )
      {
        this->IsOnlyFile = true;
      }
    }
    else if( !this->GetSingleFile() ) 
    { // not dicom
      // check the dimensions of the archetype - if there 
      // is more then one slice, use only the archetype
      // but if it is a single slice, try to generate a 
      // series of filenames
      itk::ImageFileReader<ImageType>::Pointer imageReader =
        itk::ImageFileReader<ImageType>::New();
      imageReader->SetFileName(this->Archetype);
      imageReader->UpdateOutputInformation();
      region = imageReader->GetOutput()->GetLargestPossibleRegion();
      if ( region.GetSize()[2] > 1 )
      {
        candidateFiles.push_back( this->Archetype );
        this->AllFileNames.push_back( this->Archetype );
        this->IsOnlyFile = true;
      }
      else
      {
        // Generate filenames from the Archetype
        itk::ArchetypeSeriesFileNames::Pointer fit = itk::ArchetypeSeriesFileNames::New();
        fit->SetArchetype (this->Archetype);
        candidateFiles = fit->GetFileNames();
        this->AllFileNames.resize( candidateFiles.size() );
        for (int f = 0; f < (int)(candidateFiles.size()); f ++)
        {
          this->AllFileNames[f] = candidateFiles[f];
        }
        if ( candidateFiles.size() == 1 )
        {
          this->IsOnlyFile = true;
        }
        else if ( AnalyzeHeader )
        {
          this->AnalyzeDicomHeaders();
        }
      }
    }
    else
    {
      this->AllFileNames.push_back( this->Archetype );
      this->IsOnlyFile = true;
    }
  }

  // figure out the index of Archetype in AllFileNames
  for (unsigned int k = 0; k < this->AllFileNames.size(); k++)
  {
    if (this->AllFileNames[k] == this->Archetype)
    {
      this->IndexArchetype = k;
      break;
    }
  }

  // Reduce the selection of filenames
  if ( this->IsOnlyFile || this->SingleFile )
  {
    this->FileNames.resize( 0 );
    this->FileNames.push_back( this->Archetype );
  }
  else
  {
    if ( !GroupingByTags )
    {
      AssembleVolumeContainingArchetype();
    }
    else 
    {
      GroupFiles( SelectedUID, SelectedContentTime, SelectedTriggerTime, SelectedDiffusion, SelectedSlice, SelectedOrientation );
    }
  }

  if (RasToIjkMatrix)
    {
    RasToIjkMatrix->Delete();
    }
  RasToIjkMatrix = vtkMatrix4x4::New();
  
  vtkMatrix4x4* IjkToLpsMatrix = vtkMatrix4x4::New();

  RasToIjkMatrix->Identity();
  IjkToLpsMatrix->Identity();

  double spacing[3];
  double origin[3];
  

  itk::ImageIOBase::Pointer imageIO = NULL;

  try
    {
    // If there is only one file in the series, just use an image file reader
    if (this->FileNames.size() == 1)
      {
      itk::OrientImageFilter<ImageType,ImageType>::Pointer orient =
        itk::OrientImageFilter<ImageType,ImageType>::New();
      itk::ImageFileReader<ImageType>::Pointer imageReader =
        itk::ImageFileReader<ImageType>::New();
      imageReader->SetFileName(this->FileNames[0].c_str());
      if (isDicomFile)
        {
        imageReader->SetImageIO(dicomIO);
        }

      if (this->UseNativeCoordinateOrientation)
        {
        imageReader->UpdateOutputInformation();
        filter = imageReader;
        }
      else
        {
        orient->SetInput(imageReader->GetOutput());
        orient->UseImageDirectionOn();
        orient->SetDesiredCoordinateOrientation(this->DesiredCoordinateOrientation);
        orient->UpdateOutputInformation();
        filter = orient;
        }
      for (int i = 0; i < 3; i++)
        {
        spacing[i] = filter->GetOutput()->GetSpacing()[i];
        origin[i] = filter->GetOutput()->GetOrigin()[i];

        // Get IJK to RAS direction vector
        for ( unsigned int j=0; j < filter->GetOutput()->GetImageDimension (); j++ )
          {
          IjkToLpsMatrix->SetElement(j, i, spacing[i]*filter->GetOutput()->GetDirection()[j][i]);
          }
        }
      region = filter->GetOutput()->GetLargestPossibleRegion();
      extent[0] = region.GetIndex()[0];
      extent[1] = region.GetIndex()[0] + region.GetSize()[0] - 1;
      extent[2] = region.GetIndex()[1];
      extent[3] = region.GetIndex()[1] + region.GetSize()[1] - 1;
      extent[4] = region.GetIndex()[2];
      extent[5] = region.GetIndex()[2] + region.GetSize()[2] - 1;
      imageIO = imageReader->GetImageIO();
      if (imageIO.GetPointer() == NULL) 
        {
          itkGenericExceptionMacro ( "vtkITKArchetypeImageSeriesReader::ExecuteInformation: ImageIO for file " << fileNameCollapsed.c_str() << " does not exist.");
          return;
        }
      }
    else
      {
      itk::OrientImageFilter<ImageType,ImageType>::Pointer orient =
        itk::OrientImageFilter<ImageType,ImageType>::New();
      itk::ImageSeriesReader<ImageType>::Pointer seriesReader =
        itk::ImageSeriesReader<ImageType>::New();
      seriesReader->SetFileNames(this->FileNames);
      if (isDicomFile)
        {
          seriesReader->SetImageIO(dicomIO);
        }
      else 
        {
        itk::ImageFileReader<ImageType>::Pointer imageReader =
          itk::ImageFileReader<ImageType>::New();
        imageReader->SetFileName(this->FileNames[0].c_str());
        imageReader->UpdateOutputInformation();
        imageIO = imageReader->GetImageIO();
        seriesReader->SetImageIO(imageIO);
        }
      if (this->UseNativeCoordinateOrientation)
        {
        seriesReader->UpdateOutputInformation();
        filter = seriesReader;
        }
      else
        {
        orient->SetInput(seriesReader->GetOutput());
        orient->UseImageDirectionOn();
        orient->SetDesiredCoordinateOrientation(this->DesiredCoordinateOrientation);
        orient->UpdateOutputInformation();
        filter = orient;
        }
      for (int i = 0; i < 3; i++)
        {
        spacing[i] = filter->GetOutput()->GetSpacing()[i];
        origin[i] = filter->GetOutput()->GetOrigin()[i];
        // Get IJK to RAS direction vector
        for ( unsigned int j=0; j < filter->GetOutput()->GetImageDimension (); j++ )
          {
          IjkToLpsMatrix->SetElement(j, i, spacing[i]*filter->GetOutput()->GetDirection()[j][i]);
          }
        }

      region = filter->GetOutput()->GetLargestPossibleRegion();
      extent[0] = region.GetIndex()[0];
      extent[1] = region.GetIndex()[0] + region.GetSize()[0] - 1;
      extent[2] = region.GetIndex()[1];
      extent[3] = region.GetIndex()[1] + region.GetSize()[1] - 1;
      extent[4] = region.GetIndex()[2];
      extent[5] = region.GetIndex()[2] + region.GetSize()[2] - 1;
      imageIO = seriesReader->GetImageIO();
      }
    }
    catch (...)
    {
    IjkToLpsMatrix->Delete();
    itkGenericExceptionMacro ( "vtkITKArchetypeImageSeriesReader::ExecuteInformation: Cannot open " << fileNameCollapsed.c_str() << ".");
    return;
    }
  // Transform from LPS to RAS
  vtkMatrix4x4* LpsToRasMatrix = vtkMatrix4x4::New();
  LpsToRasMatrix->Identity();
  LpsToRasMatrix->SetElement(0,0,-1);
  LpsToRasMatrix->SetElement(1,1,-1);

  vtkMatrix4x4::Multiply4x4(LpsToRasMatrix,IjkToLpsMatrix, RasToIjkMatrix);

  LpsToRasMatrix->Delete();

  // If it looks like the pipeline did not provide the spacing and
  // origin, modify the spacing and origin with the defaults
  for (int j = 0; j < 3; j++)
    {
    if (spacing[j] == 1.0)
      {
      spacing[j] = this->DefaultDataSpacing[j];
      }
    if (origin[j] == 0.0)
      {
      origin[j] = this->DefaultDataOrigin[j];
      }
    }

  origin[0] *= -1;   // L -> R
  origin[1] *= -1;   // P -> A
  
  if (this->UseNativeOrigin)
    {
      for (int j = 0; j < 3; j++)
        {
          RasToIjkMatrix->SetElement(j, 3, origin[j]);
        }
      RasToIjkMatrix->Invert();
    }
  else
    {
      RasToIjkMatrix->Invert();
      for (int j = 0; j < 3; j++)
        {
          RasToIjkMatrix->SetElement(j, 3, (extent[2*j+1] - extent[2*j])/2.0);
        }
    }

  output->SetSpacing(spacing);
  output->SetOrigin(origin);
  RasToIjkMatrix->SetElement(3,3,1.0);
  IjkToLpsMatrix->Delete();

  if ( !this->GetUseOrientationFromFile() )
  {
    RasToIjkMatrix->Identity();
    for ( unsigned int j=0; j < 3; j++ )
    {
      RasToIjkMatrix->SetElement(j, j, 1.0/spacing[j]);
    }
  }

  output->SetWholeExtent(extent);
  if (this->UseNativeScalarType)
    {
      if (imageIO.GetPointer() == NULL) 
      {
      this->SetOutputScalarType(VTK_SHORT); // TODO - figure out why multi-file series doen't have an imageIO
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::UCHAR)
      {
      this->SetOutputScalarType(VTK_UNSIGNED_CHAR);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::CHAR)
      {
      this->SetOutputScalarType(VTK_CHAR);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::USHORT)
      {
      this->SetOutputScalarType(VTK_UNSIGNED_SHORT);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::SHORT)
      {
      this->SetOutputScalarType(VTK_SHORT);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::UINT)
      {
      this->SetOutputScalarType(VTK_UNSIGNED_INT);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::INT)
      {
      this->SetOutputScalarType(VTK_INT);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::ULONG)
      {
      this->SetOutputScalarType(VTK_UNSIGNED_LONG);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::LONG)
      {
      this->SetOutputScalarType(VTK_LONG);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::FLOAT)
      {
      this->SetOutputScalarType(VTK_FLOAT);
      }
    else if (imageIO->GetComponentType() == itk::ImageIOBase::DOUBLE)
      {
      this->SetOutputScalarType(VTK_DOUBLE);
      }
    }

  if (imageIO.GetPointer() == NULL) 
    {
    this->SetNumberOfComponents(1);
    }
  else
    {
    this->SetNumberOfComponents(imageIO->GetNumberOfComponents());
    }

  output->SetScalarType(this->OutputScalarType);
  output->SetNumberOfScalarComponents(this->GetNumberOfComponents());

  // Copy the MetaDataDictionary from the ITK layer to the VTK layer
  if (imageIO.GetPointer() != NULL)
    {
    this->Dictionary = imageIO->GetMetaDataDictionary();
    }
  else
    {
    this->Dictionary = itk::MetaDataDictionary();
    }
  ParseDictionary();

}

//----------------------------------------------------------------------------
void vtkITKArchetypeImageSeriesReader::AssembleNthVolume ( int n )
{
  this->FileNames.resize( 0 );
  // int nFiles = this->AllFileNames.size(); UNUSED

  int nSlices = this->GetNumberOfSliceLocation();

  for (int k = 0; k < nSlices; k++)
  {
    const char* name = GetNthFileName( 0, -1, -1, 0, k, 0, n );
    if (name == NULL)
    {
      continue;
    }
    std::string nameInString (name);
    this->FileNames.push_back(nameInString);
  }

}


//----------------------------------------------------------------------------
const char* vtkITKArchetypeImageSeriesReader::GetNthFileName ( int idxSeriesInstanceUID,
                                                        int idxContentTime,
                                                        int idxTriggerTime,
                                                        int idxDiffusionGradientOrientation,
                                                        int idxSliceLocation,
                                                        int idxImageOrientationPatient,
                                                        int n )
{
  int nFiles = this->AllFileNames.size();
  int count = 0;
  std::string FirstName;
  for (int k = 0; k < nFiles; k++)
  {
    if ( (this->IndexSeriesInstanceUIDs[k] != idxSeriesInstanceUID && this->IndexSeriesInstanceUIDs[k] >= 0 && idxSeriesInstanceUID >= 0) ||
      (this->IndexContentTime[k] != idxContentTime && this->IndexContentTime[k] >= 0 && idxContentTime >= 0) ||
      (this->IndexTriggerTime[k] != idxTriggerTime && this->IndexTriggerTime[k] >= 0 && idxTriggerTime >= 0) ||
      (this->IndexDiffusionGradientOrientation[k] != idxDiffusionGradientOrientation  && this->IndexDiffusionGradientOrientation[k] >= 0 && idxDiffusionGradientOrientation >= 0) ||
      (this->IndexSliceLocation[k] != idxSliceLocation && this->IndexSliceLocation[k] >= 0 && idxSliceLocation >= 0) ||
      (this->IndexImageOrientationPatient[k] != idxImageOrientationPatient && this->IndexImageOrientationPatient[k] >= 0 && idxImageOrientationPatient >= 0) )
    {
      continue;
    }
    else
    {
      if (count == n)
      {
        return this->AllFileNames[k].c_str();
      }
      else if( count == 0 )
      {
        FirstName = this->AllFileNames[k];
        count ++;
      }
      else
      {
        count ++;
      }
    }
  }
  return NULL;
}

//----------------------------------------------------------------------------
void vtkITKArchetypeImageSeriesReader::GroupFiles ( int idxSeriesInstanceUID,
                                                   int idxContentTime,
                                                   int idxTriggerTime,
                                                   int idxDiffusionGradientOrientation,
                                                   int idxSliceLocation,
                                                   int idxImageOrientationPatient )
{
  this->FileNames.resize( 0 );
  int nFiles = this->AllFileNames.size();
  for (int k = 0; k < nFiles; k++)
  {
    if ( (this->IndexSeriesInstanceUIDs[k] != idxSeriesInstanceUID && this->IndexSeriesInstanceUIDs[k] >= 0 && idxSeriesInstanceUID >= 0) ||
      (this->IndexContentTime[k] != idxContentTime && this->IndexContentTime[k] >= 0 && idxContentTime >= 0) ||
      (this->IndexTriggerTime[k] != idxTriggerTime && this->IndexTriggerTime[k] >= 0 && idxTriggerTime >= 0) ||
      (this->IndexDiffusionGradientOrientation[k] != idxDiffusionGradientOrientation  && this->IndexDiffusionGradientOrientation[k] >= 0 && idxDiffusionGradientOrientation >= 0) ||
      (this->IndexSliceLocation[k] != idxSliceLocation && this->IndexSliceLocation[k] >= 0 && idxSliceLocation >= 0) ||
      (this->IndexImageOrientationPatient[k] != idxImageOrientationPatient && this->IndexImageOrientationPatient[k] >= 0 && idxImageOrientationPatient >= 0) )
    {
      continue;
    }
    else
    {
      this->FileNames.push_back( this->AllFileNames[k] );
    }
  }
  return;
}

void vtkITKArchetypeImageSeriesReader::AnalyzeDicomHeaders()
{
  itk::TimeProbe AnalyzeTime;
  AnalyzeTime.Start();

  int nFiles = this->AllFileNames.size();
  typedef itk::Image<float,3> ImageType;

  this->IndexSeriesInstanceUIDs.resize( nFiles );
  this->IndexContentTime.resize( nFiles );
  this->IndexTriggerTime.resize( nFiles );
  this->IndexDiffusionGradientOrientation.resize( nFiles );
  this->IndexSliceLocation.resize( nFiles );
  this->IndexImageOrientationPatient.resize( nFiles );

  this->SeriesInstanceUIDs.resize( 0 );
  this->ContentTime.resize( 0 );
  this->TriggerTime.resize( 0 );
  this->DiffusionGradientOrientation.resize( 0 );
  this->SliceLocation.resize( 0 );
  this->ImageOrientationPatient.resize( 0 );


  itk::GDCMImageIO::Pointer gdcmIO = itk::GDCMImageIO::New();
  if ( !gdcmIO->CanReadFile(this->Archetype) )
  {
    for (int f = 0; f < nFiles; f++)
    {
      itk::ImageFileReader<ImageType>::Pointer imageReader =
        itk::ImageFileReader<ImageType>::New();
      imageReader->SetFileName( this->AllFileNames[f] );
      imageReader->UpdateOutputInformation();

      // insert series 
      int idx = InsertSeriesInstanceUIDs( "Non-Dicom Series" );
      this->IndexSeriesInstanceUIDs[f] = idx;

      // for now, assume ContentTime, TriggerTime, and DiffusionGradientOrientation do not exist
      this->IndexContentTime[f] = -1;
      this->IndexContentTime[f] = -1;
      this->IndexDiffusionGradientOrientation[f] = -1;

      // Slice Location
      ImageType::PointType origin = imageReader->GetOutput()->GetOrigin();
      std::string IOType = imageReader->GetImageIO()->GetNameOfClass();
      if( IOType.find("BPMImageIO") == std::string::npos ||
          IOType.find("JPEGImageIO") == std::string::npos ||
          IOType.find("PNGImageIO") == std::string::npos ||
          IOType.find("TIFFImageIO") == std::string::npos ||
          IOType.find("RawImageIO") == std::string::npos )
      {
        idx = InsertSliceLocation( static_cast<float>(f) );
        this->IndexSliceLocation[f] = idx;    
      }
      else
      {
        idx = InsertSliceLocation( origin[2] );
        this->IndexSliceLocation[f] = idx;    
      }

      // Orientation
      ImageType::DirectionType orientation = imageReader->GetOutput()->GetDirection();
      float a[6];
      for (int k = 0; k < 3; k++)
      {
        a[k] = orientation[0][k];
        a[k+3] = orientation[1][k];
      }
      idx = InsertImageOrientationPatient( a );
      this->IndexImageOrientationPatient[f] = idx;    
    }
    return;
  }

  // if Archetype is a Dicom File
  gdcmIO->SetFileName( this->Archetype );
  for (int f = 0; f < nFiles; f++)
  {
    gdcmIO->SetFileName( this->AllFileNames[f] );
    gdcmIO->ReadImageInformation();
    itk::MetaDataDictionary &dict = gdcmIO->GetMetaDataDictionary();
    std::string tagValue;

    // series instance UID
    tagValue.clear(); itk::ExposeMetaData<std::string>( dict, "0020|000e", tagValue );
    if ( tagValue.length() > 0 )
    {
      int idx = InsertSeriesInstanceUIDs( tagValue.c_str() );
      this->IndexSeriesInstanceUIDs[f] = idx;
    }
    else
    {
      this->IndexSeriesInstanceUIDs[f] = -1;
    }

    // content time
    tagValue.clear(); itk::ExposeMetaData<std::string>( dict, "0008|0033", tagValue );
    if ( tagValue.length() > 0 )
    {
      int idx = InsertContentTime( tagValue.c_str() );
      this->IndexContentTime[f] = idx;
    }
    else
    {
      this->IndexContentTime[f] = -1;
    }

    // trigger time
    tagValue.clear(); itk::ExposeMetaData<std::string>( dict, "0018|1060", tagValue );
    if ( tagValue.length() > 0 )
    {
      int idx = InsertTriggerTime( tagValue.c_str() );
      this->IndexTriggerTime[f] = idx;
    }
    else
    {
      this->IndexTriggerTime[f] = -1;
    }

    // diffision gradient orientation
    tagValue.clear(); itk::ExposeMetaData<std::string>( dict, "0010|9089", tagValue );
    if ( tagValue.length() > 0 )
    {
      float a[3];
      sscanf( tagValue.c_str(), "%f\\%f\\%f", a, a+1, a+2 );
      int idx = InsertDiffusionGradientOrientation( a );
      this->IndexDiffusionGradientOrientation[f] = idx;
    }
    else
    {
      this->IndexDiffusionGradientOrientation[f] = -1;
    }

    // slice location
    tagValue.clear(); itk::ExposeMetaData<std::string>( dict, "0020|1041", tagValue );
    if ( tagValue.length() > 0 )
    {
      float a;
      sscanf( tagValue.c_str(), "%f", &a );
      int idx = InsertSliceLocation( a );
      this->IndexSliceLocation[f] = idx;
    }
    else
    {
      this->IndexSliceLocation[f] = -1;
    }

    // image orientation patient
    tagValue.clear(); itk::ExposeMetaData<std::string>( dict, "0020|0037", tagValue );
    if ( tagValue.length() > 0 )
    {
      float a[6];
      sscanf( tagValue.c_str(), "%f\\%f\\%f\\%f\\%f\\%f", a, a+1, a+2, a+3, a+4, a+5 );
      int idx = InsertImageOrientationPatient( a );
      this->IndexImageOrientationPatient[f] = idx;
    }
    else
    {
      this->IndexImageOrientationPatient[f] = -1;
    }
  }

  AnalyzeTime.Stop();

  // double timeelapsed = AnalyzeTime.GetMeanTime(); UNUSED
  AnalyzeHeader = false;
  return;
}

//----------------------------------------------------------------------------
// This function reads a data from a file.  The datas extent/axes
// are assumed to be the same as the file extent/order.
// implemented in the Scalar and Vector subclasses
void vtkITKArchetypeImageSeriesReader::ExecuteData(vtkDataObject *output)
{
  vtkErrorMacro(<<"The subclass has not defined anything for ExecuteData!\n");
}

const itk::MetaDataDictionary&
vtkITKArchetypeImageSeriesReader
::GetMetaDataDictionary() const
{
  return this->Dictionary;
}

void vtkITKArchetypeImageSeriesReader::ParseDictionary()
{
  int nItems = this->Dictionary.GetKeys().size();
  this->Tags.resize(0);
  this->TagValues.resize(0);

  if (nItems == 0)
  {
    return;
  }

  std::vector<std::string> keys = this->Dictionary.GetKeys();
  for (int k = 0; k < nItems; k++)
  {
    this->Tags.push_back( keys[k] );
    std::string tagvalue;
    itk::ExposeMetaData<std::string>(this->Dictionary, keys[k], tagvalue);
    this->TagValues.push_back( tagvalue );
  }
}

int vtkITKArchetypeImageSeriesReader::GetNumberOfItemsInDictionary()
{
  return this->Tags.size();
}

bool vtkITKArchetypeImageSeriesReader::HasKey( char* tag )
{
  return this->Dictionary.HasKey( tag ); 
}

const char* vtkITKArchetypeImageSeriesReader::GetNthKey( unsigned int n )
{
  if (n >= this->Tags.size())
  {
    return NULL;
  }
  return this->Tags[n].c_str();
}


const char* vtkITKArchetypeImageSeriesReader::GetNthValue( unsigned int n )
{
  if (n >= this->TagValues.size())
  {
    return NULL;
  }
  return this->TagValues[n].c_str();
}

const char* vtkITKArchetypeImageSeriesReader::GetTagValue( char* tag )
{
  std::string tagstr (tag);
  for (unsigned int k = 0; k < this->Tags.size(); k++)
  {
    if (this->Tags[k] == tagstr)
    {
      return this->TagValues[k].c_str();
    }
  }
  return NULL;
}

unsigned int vtkITKArchetypeImageSeriesReader::AddFileName( char* filename )
{
  std::string filenamestr (filename);
  this->FileNames.push_back( filenamestr );
  return this->FileNames.size();
}

void vtkITKArchetypeImageSeriesReader::ResetFileNames( )
{
  this->FileNames.resize( 0 );
}

int vtkITKArchetypeImageSeriesReader::AssembleVolumeContainingArchetype( )
{
  this->FileNames.resize(0);

  long int iArchetypeSeriesUID = this->IndexSeriesInstanceUIDs[this->IndexArchetype];
  long int iArchetypeContentTime = this->IndexContentTime[this->IndexArchetype];
  long int iArchetypeTriggerTime = this->IndexTriggerTime[this->IndexArchetype];
  long int iArchetypeDiffusion = this->IndexDiffusionGradientOrientation[this->IndexArchetype];
  long int iArchetypeOrientation =  this->IndexImageOrientationPatient[this->IndexArchetype];

  for (unsigned int k = 0; k < this->AllFileNames.size(); k++)
  {
    if ( (this->IndexSeriesInstanceUIDs[k] != iArchetypeSeriesUID && this->IndexSeriesInstanceUIDs[k] >= 0 && iArchetypeSeriesUID >= 0) ||
      (this->IndexContentTime[k] != iArchetypeContentTime && this->IndexContentTime[k] >= 0 && iArchetypeContentTime >= 0) ||
      (this->IndexTriggerTime[k] != iArchetypeTriggerTime && this->IndexTriggerTime[k] >= 0 && iArchetypeTriggerTime >= 0) ||
      (this->IndexDiffusionGradientOrientation[k] != iArchetypeDiffusion  && this->IndexDiffusionGradientOrientation[k] >= 0 && iArchetypeDiffusion >= 0) ||
      (this->IndexImageOrientationPatient[k] != iArchetypeOrientation && this->IndexImageOrientationPatient[k] >= 0 && iArchetypeOrientation >= 0) )
    {
      continue;
    }
    else
    {
      this->FileNames.push_back( this->AllFileNames[k] );
    }
  }

  return this->FileNames.size();
}
