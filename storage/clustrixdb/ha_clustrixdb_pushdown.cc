/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/

#include "ha_clustrixdb_pushdown.h"

/*@brief  Fills up array data types, metadata and nullability*/
/************************************************************
 * DESCRIPTION:
 * Fills up three arrays with: field binlog data types, field
 * metadata and nullability bitmask as in Table_map_log_event
 * ctor. Internally creates a temporary table as does
 * Pushdown_select.
 * More details in server/sql/log_event_server.cc
 * PARAMETERS:
 *  thd - THD*
 *  sl - SELECT_LEX*
 *  fieldtype - uchar*
 *  field_metadata - uchar*
 *  null_bits   - uchar*
 *  num_null_bytes - null bit size
 * RETURN:
 *  metadata_size int or -1 in case of error
 ************************************************************/
int get_field_types(THD *thd, SELECT_LEX *sl, uchar *fieldtype,
    uchar *field_metadata, uchar *null_bits, const int num_null_bytes)
{
  int field_metadata_size = 0;
  int metadata_index = 0;

  // Construct a tmp table with fields to find out result DTs.
  // This should be reconsidered if it worths the effort.
  List<Item> types;
  TMP_TABLE_PARAM tmp_table_param;
  sl->master_unit()->join_union_item_types(thd, types, 1);
  tmp_table_param.init();
  tmp_table_param.field_count= types.elements;

  TABLE *tmp_table = create_tmp_table(thd, &tmp_table_param, types,
                                   (ORDER *) 0, false, 0,
                                   TMP_TABLE_ALL_COLUMNS, 1,
                                   &empty_clex_str, true, false);
  if (!tmp_table) {
    field_metadata_size = -1;
    goto err;
  }

  for (unsigned int i = 0 ; i < tmp_table_param.field_count; ++i) {
    fieldtype[i]= tmp_table->field[i]->binlog_type();
  }

  bzero(field_metadata, (tmp_table_param.field_count * 2));
  for (unsigned int i= 0 ; i < tmp_table_param.field_count ; i++)
  {
    metadata_index+= tmp_table->field[i]->save_field_metadata(&field_metadata[metadata_index]);
  }

  if (metadata_index < 251)
    field_metadata_size += metadata_index + 1;
  else
    field_metadata_size += metadata_index + 3;

  bzero(null_bits, num_null_bytes);
  for (unsigned int i= 0 ; i < tmp_table_param.field_count ; ++i) {
    if (tmp_table->field[i]->maybe_null()) {
      null_bits[(i / 8)]+= 1 << (i % 8);
    }
  }

  free_tmp_table(thd, tmp_table);
err:
  return field_metadata_size;
}


/*@brief  create_clustrixdb_select_handler- Creates handler*/
/************************************************************
 * DESCRIPTION:
 * Creates a select handler
 * More details in server/sql/select_handler.h
 * PARAMETERS:
 *  thd - THD pointer.
 *  sel - SELECT_LEX* that describes the query.
 * RETURN:
 *  select_handler if possible
 *  NULL otherwise
 ************************************************************/
static select_handler*
create_clustrixdb_select_handler(THD* thd, SELECT_LEX* select_lex)
{
  ha_clustrixdb_select_handler *sh = NULL;
  String query;
  // Print the query into a string provided
  select_lex->print(thd, &query, QT_ORDINARY);
  int error_code = 0;
  clustrix_connection *clustrix_net = NULL;
  int field_metadata_size = 0;
  ulonglong scan_refid = 0;

  // We presume this number is equal to types.elements in get_field_types
  uint items_number = select_lex->get_item_list()->elements;
  uint num_null_bytes = (items_number + 7) / 8;
  uchar *fieldtype = NULL;
  uchar *null_bits = NULL;
  uchar *field_metadata = NULL;
  uchar *meta_memory= (uchar *)my_multi_malloc(MYF(MY_WME), &fieldtype, items_number,
    &null_bits, num_null_bytes, &field_metadata, (items_number * 2),
    NULL);

  if (!meta_memory) {
     // The only way to say something here is to raise warning
     // b/c we will fallback to other access methods: derived handler or rowstore.
     goto err;
  }

  if((field_metadata_size =
    get_field_types(thd, select_lex, fieldtype, field_metadata, null_bits, num_null_bytes)) < 0) {
     goto err;
  }
  // Use buffers filled by get_field_types here.

  // WIP reuse the connections
  clustrix_net = new clustrix_connection();
  error_code = clustrix_net->connect();
  if (error_code)
    goto err;

  if ((error_code = clustrix_net->scan_query_init(query, fieldtype, items_number,
        null_bits, num_null_bytes, field_metadata, field_metadata_size, &scan_refid))) {
    goto err;
  }

  sh = new ha_clustrixdb_select_handler(thd, select_lex, clustrix_net);

err:
  // reuse the connection
  if (!sh)
    delete clustrix_net;
  // deallocate buffers
  if (meta_memory)
    my_free(meta_memory);

  return sh;
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 * PARAMETERS:
 *   thd - THD pointer.
 *   select_lex - sematic tree for the query.
 **********************************************************/
ha_clustrixdb_select_handler::ha_clustrixdb_select_handler(
      THD *thd,
      SELECT_LEX* select_lex,
      clustrix_connection* clustrix_net)
  : select_handler(thd, clustrixdb_hton), clustrix_net(clustrix_net)
{
  select = select_lex;
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 **********************************************************/
ha_clustrixdb_select_handler::~ha_clustrixdb_select_handler()
{
    if (clustrix_net)
      delete clustrix_net;
}

/*@brief  Initiate the query for select_handler           */
/***********************************************************
 * DESCRIPTION:
 *  Does nothing ATM.
 *  * PARAMETERS:
 * RETURN:
 *  rc as int
 * ********************************************************/
int ha_clustrixdb_select_handler::init_scan()
{
    return 0;
}

/*@brief  Fetch next row for select_handler           */
/***********************************************************
 * DESCRIPTION:
 * Fetch next row for select_handler.
 * PARAMETERS:
 * RETURN:
 *  rc as int
 * ********************************************************/
int ha_clustrixdb_select_handler::next_row()
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(is_scan);
  assert(scan_refid);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->clustrix_net->scan_next(scan_refid, &rowdata,
                                                 &rowdata_length)))
    return error_code;

  if (has_hidden_key) {
    last_hidden_key = *(ulonglong *)rowdata;
    rowdata += 8;
    rowdata_length -= 8;
  }

  uchar const *current_row_end;
  ulong master_reclength;

  error_code = unpack_row(rgi, table, table->s->fields, rowdata,
                          &scan_fields, &current_row_end,
                          &master_reclength, rowdata + rowdata_length);

  if (error_code)
    return error_code;

  return 0;

    //return HA_ERR_END_OF_FILE;
}

/*@brief  Finishes the scan and clean it up               */
/***********************************************************
 * DESCRIPTION:
 * Finishes the scan for select handler
 * ATM this function sets vtable_state and restores it
 * afterwards since it reuses existed vtable code internally.
 * PARAMETERS:
 * RETURN:
 *   rc as int
 ***********************************************************/
int ha_clustrixdb_select_handler::end_scan()
{
/*
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  my_bitmap_free(&scan_fields);
  if (scan_refid && (error_code = trx->clustrix_net->scan_end(scan_refid)))
    return error_code;
  scan_refid = 0;

  return 0;
*/
    return 0;
}

void ha_clustrixdb_select_handler::print_error(int, unsigned long)
{
}
