import pathlib
import subprocess
import tempfile
import unittest

BIN = pathlib.Path(__file__).resolve().parents[1] / 'mini_dbms'


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.db = pathlib.Path(self.tmp.name)
        (self.db / 'people.schema').write_text('id INTEGER\nname TEXT\nage INTEGER\n')

    def run_sql(self, sql):
        path = self.db / 'query.sql'
        path.write_text(sql)
        return subprocess.run([str(BIN), str(self.db), str(path)], text=True,
                              capture_output=True)

    def test_insert_persists_and_select_reads_rows(self):
        result = self.run_sql("INSERT INTO people VALUES (1, 'Ada', 36);"
                              "INSERT INTO people VALUES (2, 'Linus', 28);")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_sql('SELECT * FROM people;')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '1,Ada,36\n2,Linus,28\n')

    def test_column_lists_map_values_and_projection_order(self):
        result = self.run_sql("insert into people (name, age, id) values ('O''Brien', 31, 7);"
                              'select name, id, name from people;')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "O'Brien,7,O'Brien\n")

    def test_omitted_id_is_generated_and_persists_for_indexed_lookup(self):
        result = self.run_sql("INSERT INTO people (name, age) VALUES ('Ada', 36);"
                              "INSERT INTO people (name, age) VALUES ('Linus', 28);")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_sql('SELECT name, age FROM people WHERE id = 2;')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'Linus,28\n')

    def test_explicit_id_advances_following_generated_id_and_id_is_unique(self):
        result = self.run_sql("INSERT INTO people VALUES (7, 'Ada', 36);"
                              "INSERT INTO people (name, age) VALUES ('Linus', 28);")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_sql('SELECT id FROM people WHERE id = 8;')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '8\n')
        result = self.run_sql("INSERT INTO people VALUES (7, 'Duplicate', 1);")
        self.assertNotEqual(result.returncode, 0)

    def test_where_comparisons_and_boolean_precedence(self):
        seed = """INSERT INTO people VALUES (1, 'Ada', 36);
        INSERT INTO people VALUES (2, 'Bob', 20);
        INSERT INTO people VALUES (3, 'Cy', 40);
        INSERT INTO people VALUES (4, 'Dee', 9);"""
        self.assertEqual(self.run_sql(seed).returncode, 0)
        cases = [
            ('age > 20 AND age <= 40', '1\n3\n'),
            ('age >= 20 AND age < 36', '2\n'),
            ("name != 'Bob' AND name < 'Dee'", '1\n3\n'),
            ('id = 1 OR id = 2 AND age > 30', '1\n'),
            ('(id = 1 OR id = 2) AND age < 30', '2\n'),
            ('age < 10', '4\n'),
        ]
        for predicate, expected in cases:
            with self.subTest(predicate=predicate):
                result = self.run_sql(f'SELECT id FROM people WHERE {predicate};')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)

    def test_csv_output_preserves_delimiters_and_quotes(self):
        result = self.run_sql("INSERT INTO people VALUES (-2, 'a,b\"c', +0009);"
                              'SELECT name, age FROM people;')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '"a,b""c",9\n')

    def test_invalid_statements_do_not_insert_rows(self):
        cases = [
            "INSERT INTO people VALUES (1, 'two\nlines', 36);",
            "INSERT INTO people VALUES (1, 'Ada', 36)",
            "INSERT INTO people VALUES (1, 'Ada', '36');",
            "INSERT INTO people VALUES (1, 'Ada', 36, 4);",
            "INSERT INTO people VALUES (1, 'Ada');",
            "INSERT INTO people (id, id, age) VALUES (1, 2, 3);",
            "INSERT INTO people (id, name) VALUES (1, 'Ada');",
            "INSERT INTO people VALUES (9223372036854775808, 'Ada', 36);",
            "INSERT INTO people VALUES (1, 'unterminated, 36);",
            "INSERT INTO ../people VALUES (1, 'Ada', 36);",
        ]
        for sql in cases:
            with self.subTest(sql=sql):
                result = self.run_sql(sql)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertEqual(self.run_sql('SELECT * FROM people;').stdout, '')

    def test_invalid_selects_fail_even_for_empty_table(self):
        for sql in ['SELECT missing FROM people;',
                    'SELECT * FROM people WHERE missing = 1;',
                    "SELECT * FROM people WHERE age = '1';",
                    'SELECT * FROM people WHERE age = 1 OR;',
                    'SELECT * FROM people WHERE (age = 1;',
                    'SELECT * FROM absent;',
                    'DELETE FROM people;']:
            with self.subTest(sql=sql):
                result = self.run_sql(sql)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)

    def test_truncated_data_is_reported(self):
        (self.db / 'people.rows').write_bytes(b'\x04\x00')
        result = self.run_sql('SELECT * FROM people;')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Corrupt', result.stderr)

    def test_earlier_statements_remain_after_later_error(self):
        result = self.run_sql("INSERT INTO people VALUES (1, 'Ada', 36);"
                              'SELECT missing FROM people;')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.run_sql('SELECT id FROM people;').stdout, '1\n')

    def test_later_lexical_error_does_not_undo_completed_insert(self):
        result = self.run_sql("INSERT INTO people VALUES (1, 'Ada', 36); 'unterminated")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.run_sql('SELECT id FROM people;').stdout, '1\n')
