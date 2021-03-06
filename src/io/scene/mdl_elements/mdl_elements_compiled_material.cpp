/***************************************************************************************************
 * Copyright (c) 2012-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************************************/

#include "pch.h"

#include "i_mdl_elements_compiled_material.h"

#include "i_mdl_elements_utilities.h"
#include "mdl_elements_utilities.h"

#include <sstream>
#include <mi/mdl/mdl_mdl.h>
#include <mi/neuraylib/icompiled_material.h>
#include <mi/neuraylib/istring.h>
#include <base/system/main/access_module.h>
#include <base/lib/config/config.h>
#include <base/lib/log/i_log_logger.h>
#include <base/util/registry/i_config_registry.h>
#include <base/data/serial/i_serializer.h>
#include <base/data/db/i_db_access.h>
#include <io/scene/scene/i_scene_journal_types.h>
#include <mdl/integration/mdlnr/i_mdlnr.h>

namespace MI {

namespace MDL {

Mdl_compiled_material::Mdl_compiled_material()
  : m_mdl_meters_per_scene_unit( 1.0f),   // avoid warning
    m_mdl_wavelength_min( 0.0f),
    m_mdl_wavelength_max( 0.0f),
    m_properties( 0)  // avoid ubsan warning with swap() and temporaries
{
    m_tf = get_type_factory();
    m_vf = get_value_factory();
    m_ef = get_expression_factory();
}

Mdl_compiled_material::Mdl_compiled_material(
    DB::Transaction* transaction,
    const mi::mdl::IGenerated_code_dag::IMaterial_instance* instance,
    const char* module_filename,
    const char* module_name,
    mi::Float32 mdl_meters_per_scene_unit,
    mi::Float32 mdl_wavelength_min,
    mi::Float32 mdl_wavelength_max)
  : m_mdl_meters_per_scene_unit( mdl_meters_per_scene_unit),
    m_mdl_wavelength_min( mdl_wavelength_min),
    m_mdl_wavelength_max( mdl_wavelength_max),
    m_properties( instance->get_properties())
{
    m_tf = get_type_factory();
    m_vf = get_value_factory();
    m_ef = get_expression_factory();

    const mi::mdl::DAG_call* constructor = instance->get_constructor();
    mi::base::Handle<IExpression> body( mdl_dag_node_to_int_expr(
        m_ef.get(),
        transaction,
        /*type_int*/ 0,
        constructor,
        /*immutable*/ true,
        /*create_direct_calls*/ true,
        module_filename,
        module_name));
    ASSERT( M_SCENE, body);
    m_body = body->get_interface<IExpression_direct_call>();
    ASSERT( M_SCENE, m_body);

    mi::Size n = instance->get_temporary_count();
    m_temporaries = m_ef->create_expression_list();
    for( mi::Size i = 0; i < n; ++i) {
        std::string name( std::to_string( i));
        const mi::mdl::DAG_node* mdl_temporary = instance->get_temporary_value( i);
        mi::base::Handle<const IExpression> temporary(
            mdl_dag_node_to_int_expr(
                m_ef.get(),
                transaction,
                /*type_int*/ 0,
                mdl_temporary,
                /*immutable*/ true,
                /*create_direct_calls*/ true,
                module_filename,
                module_name));
        ASSERT( M_SCENE, temporary);
        m_temporaries->add_expression( name.c_str(), temporary.get());
    }

    n = instance->get_parameter_count();
    m_arguments = m_vf->create_value_list();
    for( mi::Size i = 0; i < n; ++i) {
        const char* name = instance->get_parameter_name( i);
        const mi::mdl::IValue* mdl_argument = instance->get_parameter_default( i);
        mi::base::Handle<const IValue> argument( mdl_value_to_int_value(
            m_vf.get(), transaction, 0, mdl_argument, module_filename, module_name));
        ASSERT( M_SCENE, argument);
        m_arguments->add_value( name, argument.get());
    }

    const mi::mdl::DAG_hash* h = instance->get_hash();
    m_hash = convert_hash( *h);

    for( int i = 0; i <= mi::mdl::IGenerated_code_dag::IMaterial_instance::MS_LAST; ++i) {
        h = instance->get_slot_hash(
            static_cast<mi::mdl::IGenerated_code_dag::IMaterial_instance::Slot>( i));
        m_slot_hashes[i] = convert_hash( *h);
    }
}

const IExpression_direct_call* Mdl_compiled_material::get_body() const
{
    m_body->retain();
    return m_body.get();
}

mi::Size Mdl_compiled_material::get_temporary_count() const
{
    return m_temporaries->get_size();
}

const IExpression* Mdl_compiled_material::get_temporary( mi::Size index) const
{
    return m_temporaries->get_expression( index);
}

mi::Float32 Mdl_compiled_material::get_mdl_meters_per_scene_unit() const
{
    return m_mdl_meters_per_scene_unit;
}

mi::Float32 Mdl_compiled_material::get_mdl_wavelength_min() const
{
    return m_mdl_wavelength_min;
}

mi::Float32 Mdl_compiled_material::get_mdl_wavelength_max() const
{
    return m_mdl_wavelength_max;
}

bool Mdl_compiled_material::depends_on_state_transform() const
{
    return 0 !=
        (m_properties & mi::mdl::IGenerated_code_dag::IMaterial_instance::IP_DEPENDS_ON_TRANSFORM);
}

bool Mdl_compiled_material::depends_on_state_object_id() const
{
    return 0 !=
        (m_properties & mi::mdl::IGenerated_code_dag::IMaterial_instance::IP_DEPENDS_ON_OBJECT_ID);
}

bool Mdl_compiled_material::depends_on_global_distribution() const
{
    return 0 !=
        (m_properties &
         mi::mdl::IGenerated_code_dag::IMaterial_instance::IP_DEPENDS_ON_GLOBAL_DISTRIBUTION);
}

mi::Size Mdl_compiled_material::get_parameter_count() const
{
    return m_arguments->get_size();
}

char const* Mdl_compiled_material::get_parameter_name( mi::Size index) const
{
    return m_arguments->get_name( index);
}

const IValue* Mdl_compiled_material::get_argument( mi::Size index) const
{
    return m_arguments->get_value( index);
}

mi::base::Uuid Mdl_compiled_material::get_hash() const
{
    return m_hash;
}

mi::base::Uuid Mdl_compiled_material::get_slot_hash( mi::Uint32 slot) const
{
    typedef mi::mdl::IGenerated_code_dag::IMaterial_instance T;

    switch( slot) {
        case mi::neuraylib::SLOT_THIN_WALLED:
            return m_slot_hashes[T::MS_THIN_WALLED];
        case mi::neuraylib::SLOT_SURFACE_SCATTERING:
            return m_slot_hashes[T::MS_SURFACE_BSDF_SCATTERING];
        case mi::neuraylib::SLOT_SURFACE_EMISSION_EDF_EMISSION:
            return m_slot_hashes[T::MS_SURFACE_EMISSION_EDF_EMISSION];
        case mi::neuraylib::SLOT_SURFACE_EMISSION_INTENSITY:
            return m_slot_hashes[T::MS_SURFACE_EMISSION_INTENSITY];
        case mi::neuraylib::SLOT_BACKFACE_SCATTERING:
            return m_slot_hashes[T::MS_BACKFACE_BSDF_SCATTERING];
        case mi::neuraylib::SLOT_BACKFACE_EMISSION_EDF_EMISSION:
            return m_slot_hashes[T::MS_BACKFACE_EMISSION_EDF_EMISSION];
        case mi::neuraylib::SLOT_BACKFACE_EMISSION_INTENSITY:
            return m_slot_hashes[T::MS_BACKFACE_EMISSION_INTENSITY];
        case mi::neuraylib::SLOT_IOR:
            return m_slot_hashes[T::MS_IOR];
        case mi::neuraylib::SLOT_VOLUME_SCATTERING:
            return m_slot_hashes[T::MS_VOLUME_VDF_SCATTERING];
        case mi::neuraylib::SLOT_VOLUME_ABSORPTION_COEFFICIENT:
            return m_slot_hashes[T::MS_VOLUME_ABSORPTION_COEFFICIENT];
        case mi::neuraylib::SLOT_VOLUME_SCATTERING_COEFFICIENT:
            return m_slot_hashes[T::MS_VOLUME_SCATTERING_COEFFICIENT];
        case mi::neuraylib::SLOT_GEOMETRY_DISPLACEMENT:
            return m_slot_hashes[T::MS_GEOMETRY_DISPLACEMENT];
        case mi::neuraylib::SLOT_GEOMETRY_CUTOUT_OPACITY:
            return m_slot_hashes[T::MS_GEOMETRY_CUTOUT_OPACITY];
        case mi::neuraylib::SLOT_GEOMETRY_NORMAL:
            return m_slot_hashes[T::MS_GEOMETRY_NORMAL];
        default:
            return mi::base::Uuid();
    }
}

const IExpression_list* Mdl_compiled_material::get_temporaries() const
{
    m_temporaries->retain();
    return m_temporaries.get();
}

const IValue_list* Mdl_compiled_material::get_arguments() const
{
    m_arguments->retain();
    return m_arguments.get();
}

void Mdl_compiled_material::swap( Mdl_compiled_material& other)
{
    SCENE::Scene_element<Mdl_compiled_material, ID_MDL_COMPILED_MATERIAL>::swap(
        other);

    m_body.swap( other.m_body);
    m_temporaries.swap( other.m_temporaries);
    m_arguments.swap( other.m_arguments);

    std::swap( m_hash, other.m_hash);
    for( int i = 0; i < mi::mdl::IGenerated_code_dag::IMaterial_instance::MS_LAST+1; ++i)
        std::swap( m_slot_hashes[i], other.m_slot_hashes[i]);
    std::swap( m_mdl_meters_per_scene_unit, other.m_mdl_meters_per_scene_unit);
    std::swap( m_mdl_wavelength_min, other.m_mdl_wavelength_min);
    std::swap( m_mdl_wavelength_max, other.m_mdl_wavelength_max);
    std::swap( m_properties, other.m_properties);
}

const IExpression* Mdl_compiled_material::lookup_sub_expression(
    DB::Transaction* transaction,
    const char* path,
    mi::mdl::IType_factory* tf,
    const mi::mdl::IType** sub_type) const
{
    ASSERT( M_SCENE, path);
    ASSERT( M_SCENE, (!transaction && !tf && !sub_type) || (transaction && tf && sub_type));

    const mi::mdl::IType_struct* material_type
        = tf ? tf->get_predefined_struct( mi::mdl::IType_struct::SID_MATERIAL) : 0;

    return MDL::lookup_sub_expression(
        transaction, m_ef.get(), m_temporaries.get(), material_type, m_body.get(),
        path, sub_type);
}

const IExpression* Mdl_compiled_material::lookup_sub_expression( const char* path) const
{
    return lookup_sub_expression( 0, path, 0, 0);
}

namespace {

void write( SERIAL::Serializer* serializer, const mi::base::Uuid& uuid)
{
    serializer->write( uuid.m_id1);
    serializer->write( uuid.m_id2);
    serializer->write( uuid.m_id3);
    serializer->write( uuid.m_id4);
}

void read( SERIAL::Deserializer* deserializer, mi::base::Uuid& uuid)
{
    deserializer->read( &uuid.m_id1);
    deserializer->read( &uuid.m_id2);
    deserializer->read( &uuid.m_id3);
    deserializer->read( &uuid.m_id4);
}

}

const SERIAL::Serializable* Mdl_compiled_material::serialize(
    SERIAL::Serializer* serializer) const
{
    Scene_element_base::serialize( serializer);

    m_ef->serialize( serializer, m_body.get());
    m_ef->serialize_list( serializer, m_temporaries.get());
    m_vf->serialize_list( serializer, m_arguments.get());

    write( serializer, m_hash);
    for( int i = 0; i < mi::mdl::IGenerated_code_dag::IMaterial_instance::MS_LAST+1; ++i)
        write( serializer, m_slot_hashes[i]);

    serializer->write( m_mdl_meters_per_scene_unit);
    serializer->write( m_mdl_wavelength_min);
    serializer->write( m_mdl_wavelength_max);
    serializer->write( m_properties);
    return this + 1;
}

SERIAL::Serializable* Mdl_compiled_material::deserialize(
    SERIAL::Deserializer* deserializer)
{
    Scene_element_base::deserialize( deserializer);

    mi::base::Handle<IExpression> body( m_ef->deserialize( deserializer));
    m_body        = body->get_interface<IExpression_direct_call>();
    m_temporaries = m_ef->deserialize_list( deserializer);
    m_arguments   = m_vf->deserialize_list( deserializer);

    read( deserializer, m_hash);
    for( int i = 0; i < mi::mdl::IGenerated_code_dag::IMaterial_instance::MS_LAST+1; ++i)
        read( deserializer, m_slot_hashes[i]);

    deserializer->read( &m_mdl_meters_per_scene_unit);
    deserializer->read( &m_mdl_wavelength_min);
    deserializer->read( &m_mdl_wavelength_max);
    deserializer->read( &m_properties);
    return this + 1;
}

void Mdl_compiled_material::dump( DB::Transaction* transaction) const
{
    std::ostringstream s;
    s << std::boolalpha;
    mi::base::Handle<const mi::IString> tmp;

    tmp = m_vf->dump( transaction, m_arguments.get(), /*name*/ 0);
    s << "Arguments: " << tmp->get_c_str() << std::endl;

    tmp = m_ef->dump( transaction, m_temporaries.get(), /*name*/ 0);
    s << "Temporaries: " << tmp->get_c_str() << std::endl;

    tmp = m_ef->dump( transaction, m_body.get(), /*name*/ 0);
    s << "Body: " << tmp->get_c_str() << std::endl;

    char buffer[36];
    snprintf( buffer, sizeof( buffer),
        "%08x %08x %08x %08x", m_hash.m_id1, m_hash.m_id2, m_hash.m_id3, m_hash.m_id4);
    s << "Hash: " << buffer << std::endl;

    for( mi::Size i = 0; i < mi::mdl::IGenerated_code_dag::IMaterial_instance::MS_LAST+1; ++i) {
        const mi::base::Uuid& hash = m_slot_hashes[i];
        snprintf( buffer, sizeof( buffer),
            "%08x %08x %08x %08x", hash.m_id1, hash.m_id2, hash.m_id3, hash.m_id4);
        s << "Slot hash[" << i << "]: " << buffer << std::endl;

    }
    s << "Meters per scene unit: " << m_mdl_meters_per_scene_unit << std::endl;
    s << "Wavelength min: " << m_mdl_wavelength_min << std::endl;
    s << "Wavelength max: " << m_mdl_wavelength_max << std::endl;
    s << "Properties: " << m_properties << std::endl;
    LOG::mod_log->info( M_SCENE, LOG::Mod_log::C_DATABASE, "%s", s.str().c_str());
}

size_t Mdl_compiled_material::get_size() const
{
    return sizeof( *this)
        + SCENE::Scene_element<Mdl_compiled_material, Mdl_compiled_material::id>::get_size()
            - sizeof( SCENE::Scene_element<Mdl_compiled_material, Mdl_compiled_material::id>)
        + dynamic_memory_consumption( m_body)
        + dynamic_memory_consumption( m_temporaries)
        + dynamic_memory_consumption( m_arguments);
}

DB::Journal_type Mdl_compiled_material::get_journal_flags() const
{
    return DB::JOURNAL_NONE;
}

Uint Mdl_compiled_material::bundle( DB::Tag* results, Uint size) const
{
    return 0;
}

void Mdl_compiled_material::get_scene_element_references( DB::Tag_set* result) const
{
    collect_references( m_body.get(), result);
    collect_references( m_temporaries.get(), result);
    collect_references( m_arguments.get(), result);
}

mi::base::Uuid Mdl_compiled_material::convert_hash(
    const mi::mdl::DAG_hash& h)
{
    mi::base::Uuid result;
    result.m_id1 = (h[ 0] << 24) | (h[ 1] << 16) | (h[ 2] << 8) | h[ 3];
    result.m_id2 = (h[ 4] << 24) | (h[ 5] << 16) | (h[ 6] << 8) | h[ 7];
    result.m_id3 = (h[ 8] << 24) | (h[ 9] << 16) | (h[10] << 8) | h[11];
    result.m_id4 = (h[12] << 24) | (h[13] << 16) | (h[14] << 8) | h[15];
    return result;
}

} // namespace MDL

} // namespace MI

