/***************************************************************************
    qgsgrassimport.cpp  -  Import to GRASS mapset
                             -------------------
    begin                : May, 2015
    copyright            : (C) 2015 Radim Blazek
    email                : radim.blazek@gmail.com
 ***************************************************************************/
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include <unistd.h>

#include <QByteArray>
#include <QtConcurrentRun>

#include "qgscoordinatereferencesystem.h"
#include "qgscoordinatetransform.h"
#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsgeometry.h"
#include "qgsproviderregistry.h"
#include "qgsrasterdataprovider.h"
#include "qgsrasteriterator.h"

#include "qgsgrassimport.h"

extern "C"
{
#include <grass/version.h>
#include <grass/gis.h>
#include <grass/raster.h>
}

QgsGrassImport::QgsGrassImport( QgsGrassObject grassObject )
    : QObject()
    , mGrassObject( grassObject )
    , mCanceled( false )
    , mFutureWatcher( 0 )
{
}

QgsGrassImport::~QgsGrassImport()
{
  if ( mFutureWatcher && !mFutureWatcher->isFinished() )
  {
    QgsDebugMsg( "mFutureWatcher not finished -> waitForFinished()" );
    mFutureWatcher->waitForFinished();
  }
}

void QgsGrassImport::setError( QString error )
{
  QgsDebugMsg( "error: " + error );
  mError = error;
}

QString QgsGrassImport::error()
{
  return mError;
}

void QgsGrassImport::importInThread()
{
  QgsDebugMsg( "entered" );
  mFutureWatcher = new QFutureWatcher<bool>( this );
  connect( mFutureWatcher, SIGNAL( finished() ), SLOT( onFinished() ) );
  mFutureWatcher->setFuture( QtConcurrent::run( run, this ) );
}

bool QgsGrassImport::run( QgsGrassImport *imp )
{
  QgsDebugMsg( "entered" );
  imp->import();
  return true;
}

void QgsGrassImport::onFinished()
{
  QgsDebugMsg( "entered" );
  emit finished( this );
}

QStringList QgsGrassImport::names() const
{
  QStringList list;
  list << mGrassObject.name();
  return list;
}

//------------------------------ QgsGrassRasterImport ------------------------------------
QgsGrassRasterImport::QgsGrassRasterImport( QgsRasterPipe* pipe, const QgsGrassObject& grassObject,
    const QgsRectangle &extent, int xSize, int ySize )
    : QgsGrassImport( grassObject )
    , mPipe( pipe )
    , mExtent( extent )
    , mXSize( xSize )
    , mYSize( ySize )
{
}

QgsGrassRasterImport::~QgsGrassRasterImport()
{
  if ( mFutureWatcher && !mFutureWatcher->isFinished() )
  {
    QgsDebugMsg( "mFutureWatcher not finished -> waitForFinished()" );
    mFutureWatcher->waitForFinished();
  }
  delete mPipe;
}

bool QgsGrassRasterImport::import()
{
  QgsDebugMsg( "entered" );
  if ( !mPipe )
  {
    setError( "Pipe is null." );
    return false;
  }

  QgsRasterDataProvider * provider = mPipe->provider();
  if ( !provider )
  {
    setError( "Pipe has no provider." );
    return false;
  }

  if ( !provider->isValid() )
  {
    setError( "Provider is not valid." );
    return false;
  }

  for ( int band = 1; band <= provider->bandCount(); band++ )
  {
    QgsDebugMsg( QString( "band = %1" ).arg( band ) );
    QGis::DataType qgis_out_type = QGis::UnknownDataType;
    RASTER_MAP_TYPE data_type = -1;
    switch ( provider->dataType( band ) )
    {
      case QGis::Byte:
      case QGis::UInt16:
      case QGis::Int16:
      case QGis::UInt32:
      case QGis::Int32:
        qgis_out_type = QGis::Int32;
        break;
      case QGis::Float32:
        qgis_out_type = QGis::Float32;
        break;
      case QGis::Float64:
        qgis_out_type = QGis::Float64;
        break;
      case QGis::ARGB32:
      case QGis::ARGB32_Premultiplied:
        qgis_out_type = QGis::Int32;  // split to multiple bands?
        break;
      case QGis::CInt16:
      case QGis::CInt32:
      case QGis::CFloat32:
      case QGis::CFloat64:
      case QGis::UnknownDataType:
        setError( tr( "Data type %1 not supported" ).arg( provider->dataType( band ) ) );
        return false;
    }

    QgsDebugMsg( QString( "data_type = %1" ).arg( data_type ) );

    QString module = QgsGrass::qgisGrassModulePath() + "/qgis.r.in";
    QStringList arguments;
    QString name = mGrassObject.name();
    if ( provider->bandCount() > 1 )
    {
      name += QString( "_%1" ).arg( band );
    }
    arguments.append( "output=" + name );    // get list of all output names
    QTemporaryFile gisrcFile;
    QProcess* process = 0;
    try
    {
      process = QgsGrass::startModule( mGrassObject.gisdbase(), mGrassObject.location(), mGrassObject.mapset(), module, arguments, gisrcFile );
    }
    catch ( QgsGrass::Exception &e )
    {
      setError( e.what() );
      return false;
    }

    QDataStream outStream( process );

    outStream << mExtent << ( qint32 )mXSize << ( qint32 )mYSize;
    outStream << ( qint32 )qgis_out_type;

    // calculate reasonable block size (5MB)
    int maximumTileHeight = 5000000 / mXSize;
    maximumTileHeight = std::max( 1, maximumTileHeight );

    QgsRasterIterator iter( mPipe->last() );
    iter.setMaximumTileWidth( mXSize );
    iter.setMaximumTileHeight( maximumTileHeight );

    iter.startRasterRead( band, mXSize, mYSize, mExtent );

    int iterLeft = 0;
    int iterTop = 0;
    int iterCols = 0;
    int iterRows = 0;
    QgsRasterBlock* block = 0;
    while ( iter.readNextRasterPart( band, iterCols, iterRows, &block, iterLeft, iterTop ) )
    {
      for ( int row = 0; row < iterRows; row++ )
      {
        if ( !block->convert( qgis_out_type ) )
        {
          setError( "cannot vonvert data type" );
          delete block;
          return false;
        }
        char * data = block->bits( row, 0 );
        int size = iterCols * block->dataTypeSize();
        QByteArray byteArray = QByteArray::fromRawData( data, size ); // does not copy data and does not take ownership
        if ( isCanceled() )
        {
          outStream << true; // cancel module
          break;
        }
        outStream << false; // not canceled
        outStream << byteArray;
      }
      delete block;
      if ( isCanceled() )
      {
        outStream << true; // cancel module
        break;
      }
    }

    // TODO: send something back from module and read it here to close map correctly in module

    process->closeWriteChannel();
    // TODO: best timeout?
    process->waitForFinished( 30000 );

    QString stdoutString = process->readAllStandardOutput().data();
    QString stderrString = process->readAllStandardError().data();

    QString processResult = QString( "exitStatus=%1, exitCode=%2, errorCode=%3, error=%4 stdout=%5, stderr=%6" )
                            .arg( process->exitStatus() ).arg( process->exitCode() )
                            .arg( process->error() ).arg( process->errorString() )
                            .arg( stdoutString ).arg( stderrString );
    QgsDebugMsg( "processResult: " + processResult );

    if ( process->exitStatus() != QProcess::NormalExit )
    {
      setError( process->errorString() );
      delete process;
      return false;
    }

    if ( process->exitCode() != 0 )
    {
      setError( stderrString );
      delete process;
      return false;
    }

    delete process;
  }
  return true;
}

QString QgsGrassRasterImport::srcDescription() const
{
  if ( !mPipe || !mPipe->provider() )
  {
    return "";
  }
  return mPipe->provider()->dataSourceUri();
}

QStringList QgsGrassRasterImport::extensions( QgsRasterDataProvider* provider )
{
  QStringList list;
  if ( provider && provider->bandCount() > 1 )
  {
    for ( int band = 1; band <= provider->bandCount(); band++ )
    {
      list << QString( "_%1" ).arg( band );
    }
  }
  return list;
}

QStringList QgsGrassRasterImport::names() const
{
  QStringList list;
  if ( mPipe && mPipe->provider() )
  {
    foreach ( QString ext, extensions( mPipe->provider() ) )
    {
      list << mGrassObject.name() + ext;
    }
  }
  if ( list.isEmpty() )
  {
    list << mGrassObject.name();
  }
  return list;
}

//------------------------------ QgsGrassVectorImport ------------------------------------
QgsGrassVectorImport::QgsGrassVectorImport( QgsVectorDataProvider* provider, const QgsGrassObject& grassObject )
    : QgsGrassImport( grassObject )
    , mProvider( provider )
{
}

QgsGrassVectorImport::~QgsGrassVectorImport()
{
  if ( mFutureWatcher && !mFutureWatcher->isFinished() )
  {
    QgsDebugMsg( "mFutureWatcher not finished -> waitForFinished()" );
    mFutureWatcher->waitForFinished();
  }
  delete mProvider;
}

bool QgsGrassVectorImport::import()
{
  QgsDebugMsg( "entered" );

  if ( !mProvider )
  {
    setError( "Provider is null." );
    return false;
  }

  if ( !mProvider->isValid() )
  {
    setError( "Provider is not valid." );
    return false;
  }

  QgsCoordinateReferenceSystem providerCrs = mProvider->crs();
  QgsCoordinateReferenceSystem mapsetCrs = QgsGrass::crsDirect( mGrassObject.gisdbase(), mGrassObject.location() );
  QgsDebugMsg( "providerCrs = " + providerCrs.toWkt() );
  QgsDebugMsg( "mapsetCrs = " + mapsetCrs.toWkt() );
  QgsCoordinateTransform coordinateTransform;
  bool doTransform = false;
  if ( providerCrs.isValid() && mapsetCrs.isValid() && providerCrs != mapsetCrs )
  {
    coordinateTransform.setSourceCrs( providerCrs );
    coordinateTransform.setDestCRS( mapsetCrs );
    doTransform = true;
  }

  QString module = QgsGrass::qgisGrassModulePath() + "/qgis.v.in";
  QStringList arguments;
  QString name = mGrassObject.name();
  arguments.append( "output=" + name );
  QTemporaryFile gisrcFile;
  QProcess* process = 0;
  try
  {
    process = QgsGrass::startModule( mGrassObject.gisdbase(), mGrassObject.location(), mGrassObject.mapset(), module, arguments, gisrcFile );
  }
  catch ( QgsGrass::Exception &e )
  {
    setError( e.what() );
    return false;
  }

  QDataStream outStream( process );

  QGis::WkbType wkbType = mProvider->geometryType();
  bool isPolygon = QGis::singleType( QGis::flatType( wkbType ) ) == QGis::WKBPolygon;
  outStream << ( qint32 )wkbType;

  outStream << mProvider->fields();

  QgsFeatureIterator iterator = mProvider->getFeatures();
  QgsFeature feature;
  for ( int i = 0; i < ( isPolygon ? 2 : 1 ); i++ ) // two cycles with polygons
  {
    if ( i > 0 )
    {
      //iterator.rewind(); // rewind does not work
      iterator = mProvider->getFeatures();
    }
    QgsDebugMsg( "send features" );
    while ( iterator.nextFeature( feature ) )
    {
      if ( !feature.isValid() )
      {
        continue;
      }
      if ( doTransform && feature.geometry() )
      {
        feature.geometry()->transform( coordinateTransform );
      }
      if ( isCanceled() )
      {
        outStream << true; // cancel module
        break;
      }
      outStream << false; // not canceled
      outStream << feature;
    }
    feature = QgsFeature(); // indicate end by invalid feature
    outStream << false; // not canceled
    outStream << feature;
    QgsDebugMsg( "features sent" );
  }
  iterator.close();

  process->setReadChannel( QProcess::StandardOutput );

  bool result;
  outStream >> result;
  QgsDebugMsg( QString( "result = %1" ).arg( result ) );

  process->closeWriteChannel();
  process->waitForFinished( 5000 );

  QString stdoutString = process->readAllStandardOutput().data();
  QString stderrString = process->readAllStandardError().data();

  QString processResult = QString( "exitStatus=%1, exitCode=%2, errorCode=%3, error=%4 stdout=%5, stderr=%6" )
                          .arg( process->exitStatus() ).arg( process->exitCode() )
                          .arg( process->error() ).arg( process->errorString() )
                          .arg( stdoutString ).arg( stderrString );
  QgsDebugMsg( "processResult: " + processResult );

  if ( process->exitStatus() != QProcess::NormalExit )
  {
    setError( process->errorString() );
    delete process;
    return false;
  }

  if ( process->exitCode() != 0 )
  {
    setError( stderrString );
    delete process;
    return false;
  }

  delete process;
  return true;
}

QString QgsGrassVectorImport::srcDescription() const
{
  if ( !mProvider )
  {
    return "";
  }
  return mProvider->dataSourceUri();
}

//------------------------------ QgsGrassCopy ------------------------------------
QgsGrassCopy::QgsGrassCopy( const QgsGrassObject& srcObject, const QgsGrassObject& destObject )
    : QgsGrassImport( destObject )
    , mSrcObject( srcObject )
{
}

QgsGrassCopy::~QgsGrassCopy()
{
}

bool QgsGrassCopy::import()
{
  QgsDebugMsg( "entered" );

  try
  {
    QgsGrass::copyObject( mSrcObject, mGrassObject );
  }
  catch ( QgsGrass::Exception &e )
  {
    setError( e.what() );
    return false;
  }

  return true;
}


QString QgsGrassCopy::srcDescription() const
{
  return mSrcObject.toString();
}
