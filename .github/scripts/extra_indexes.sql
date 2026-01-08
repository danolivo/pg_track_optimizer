-- Extra indexes for JO-Bench to reduce filtered tuples
-- Source: https://github.com/danolivo/pg_track_optimizer/wiki/Extra-Indexing
--
-- These indexes are applied in the second pass of JO-Bench testing
-- to measure the impact of indexing on cardinality estimation errors.

CREATE INDEX aka_name_idx_2 ON aka_name (name);
CREATE INDEX movie_info_idx_idx_1 ON movie_info_idx (info);
CREATE INDEX title_idx_1 ON title (production_year);

CREATE EXTENSION IF NOT EXISTS pg_trgm;

CREATE INDEX idx_movie_companies ON movie_companies USING gin (note gin_trgm_ops);
CREATE INDEX cast_info_idx_1 ON cast_info USING gin (note gin_trgm_ops);
CREATE INDEX idx_movie_info ON movie_info USING gin (info gin_trgm_ops);
CREATE INDEX keyword_idx_1 ON keyword USING gin (keyword gin_trgm_ops);
CREATE INDEX info_type_idx_1 ON info_type USING gin (info gin_trgm_ops);
CREATE INDEX company_name_idx_1 ON company_name USING gin (country_code gin_trgm_ops);
