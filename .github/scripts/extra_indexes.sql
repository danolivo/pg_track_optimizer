-- Extra indexes for JO-Bench to reduce filtered tuples
-- Source: https://github.com/danolivo/pg_track_optimizer/wiki/Extra-Indexing
--
-- These indexes are applied in the second pass of JO-Bench testing
-- to measure the impact of indexing on cardinality estimation errors.

CREATE INDEX aka_name_idx_1 ON aka_name (info);
CREATE INDEX aka_name_idx_2 ON aka_name (name);
CREATE INDEX aka_title_idx_1 ON aka_title (info,note);
CREATE INDEX cast_info_idx_1 ON cast_info (note);
CREATE INDEX company_name_idx_1 ON company_name (country_code);
CREATE INDEX complete_cast_idx_1 ON complete_cast (info);
CREATE INDEX complete_cast_idx_2 ON complete_cast (info,note);
CREATE INDEX info_type_idx_1 ON info_type (info);
CREATE INDEX keyword_idx_1 ON keyword (keyword);
CREATE INDEX movie_companies_idx_1 ON movie_companies (country_code);
CREATE INDEX movie_companies_idx_2 ON movie_companies (info);
CREATE INDEX movie_companies_idx_3 ON movie_companies (info,note);
CREATE INDEX movie_companies_idx_4 ON movie_companies (note);
CREATE INDEX movie_info_idx_1 ON movie_info (info);
CREATE INDEX movie_info_idx_idx_1 ON movie_info_idx (info);
CREATE INDEX movie_keyword_idx_1 ON movie_keyword (info);
CREATE INDEX movie_keyword_idx_2 ON movie_keyword (keyword);
CREATE INDEX movie_keyword_idx_3 ON movie_keyword (note);
CREATE INDEX movie_link_idx_1 ON movie_link (note);
CREATE INDEX title_idx_1 ON title (production_year);
